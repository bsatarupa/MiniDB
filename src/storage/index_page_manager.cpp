#include "minidb/storage/index_page_manager.h"
#include "minidb/storage/disk_manager.h"
#include "minidb/storage/page_format.h"
#include <cstdint>
#include <mutex>
#include <stdexcept>

namespace minidb::storage {

    //a new index starts with root_page_id = INVALID_PAGE_ID, first allocatable page_id = 1,
    //initial metadata persisted to page_id = 0. When reopening an existing index,
    //constructor restores thr root_page_id and next_page_id from its metapage.
    IndexPageManager::IndexPageManager(DiskManager &disk) :
                        disk(disk), root_page_id(format::INVALID_PAGE_ID), next_page_id(1) {
            
        //constructor does not need mutex locking because the object is not accessible to other threads
        //until constructor finishes
        using namespace format;

        if (disk.is_empty()) {
            //case1: Initialize metadata with an empty tree - create a new Index file and flush to disk
            persist_metadata();
            return;
        }

        //case2: Read existing metadata from disk and restore the root_page_id, 
        //next_page_id and allocation state - Existing Index file
        const auto metadata = disk.read_page(META_PAGE_ID);
        read_index_metadata_page(metadata, root_page_id, next_page_id);
    }

    std::uint64_t IndexPageManager::allocate_page() {

        std::lock_guard<std::mutex> lock(mtx); //locks Index Manager's mutex

        using namespace format;

        if (next_page_id == INVALID_PAGE_ID) {
            throw std::overflow_error("Index pageID space exhausted");
        }

        //write current page data and next page metadata to disk
        const std::uint64_t page_id = next_page_id; //reserve space for current 'next_page_id'
        const auto page = make_data_page(page_id, PageType::FREE); //Initializes the page as FREE
        disk.write_page(page_id, page);

        const std::uint64_t updated_next_page_id = next_page_id + 1;
        const auto metadata = make_index_metadata_page(root_page_id, updated_next_page_id);
        disk.write_page(META_PAGE_ID, metadata);    //write updated index metadata to page_0

        disk.flush(); //flushes the file

        next_page_id = updated_next_page_id;    //undate counter only after the writes to disk succeed
        return page_id; //pageID of allocated page
    }

    std::uint64_t IndexPageManager::get_root_page_id() const {

        std::lock_guard<std::mutex> lock(mtx);
        return root_page_id;
    }

    void IndexPageManager::set_root_page_id(std::uint64_t page_id) {
        
        std::lock_guard<std::mutex> lock(mtx);  //locks Index Manager's mutex

        using namespace format;

        if (page_id != INVALID_PAGE_ID && (page_id == META_PAGE_ID || page_id >= next_page_id)) {
            //reject page_0 or pageIDs that have not been allocated
            throw std::out_of_range("Root PageID is not allocated");
        }

        const std::uint64_t previous_root_page_id = root_page_id;   //stored for backtracking
        
        root_page_id = page_id; //asign and attempt to persist
        try {
            persist_metadata();
        } catch (...) {
            root_page_id = previous_root_page_id;
            throw;
        }
    }

    DiskManager::Page IndexPageManager::read_page(std::uint64_t page_id) {

        std::lock_guard<std::mutex> lock(mtx);  //locks Index Manager's mutex

        using namespace format;

        if (page_id == META_PAGE_ID || page_id >= next_page_id) { //reject page_0 or pageIDs that have not been allocated
            throw std::out_of_range("Index pageID is not allocated");
        }

        auto page = disk.read_page(page_id);
        validate_data_page(page, page_id);  //validate pageID, pageType, formatVersion after reading

        return page;
    }

    void IndexPageManager::write_page(std::uint64_t page_id, const DiskManager::Page &page) {

        std::lock_guard<std::mutex> lock(mtx);  //locks Index Manager's mutex

        using namespace format;

        if (page_id == META_PAGE_ID || page_id >= next_page_id) { //reject page_0 or pageIDs that have not been allocated
            throw std::out_of_range("Index pageID is not allocated");
        }

        validate_data_page(page, page_id);  //validate pageID, pageType, formatVersion before writing
        disk.write_page(page_id, page);

        disk.flush();
    }

    void IndexPageManager::persist_metadata() {

        //No need to add lock_guard<mutex>, already acquired by its callers

        using namespace format; //page_format.h

        //serialize root_page_id and next_page_id into an index_metadata page
        const auto metadata = make_index_metadata_page(root_page_id, next_page_id);

        //write that index_metadata page to page_0 of the index file
        disk.write_page(META_PAGE_ID, metadata);

        //flush the file to underlying storage layer
        disk.flush();
    }

    

} //namespace metadata::storage