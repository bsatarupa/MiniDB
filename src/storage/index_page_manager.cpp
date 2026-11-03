#include "minidb/storage/index_page_manager.h"
#include "minidb/storage/disk_manager.h"
#include "minidb/storage/page_format.h"
#include "minidb/storage/wal/log_manager.h"
#include "minidb/storage/wal/log_record.h"
#include <cstdint>
#include <mutex>
#include <stdexcept>

namespace minidb::storage {

    //a new index starts with root_page_id = INVALID_PAGE_ID, first allocatable page_id = 1,
    //initial metadata persisted to page_id = 0. When reopening an existing index,
    //constructor restores thr root_page_id and next_page_id from its metapage.
    IndexPageManager::IndexPageManager(DiskManager &disk) :
                        disk(disk), root_page_id(format::INVALID_PAGE_ID), 
                        next_page_id(1), metadata_lsn(format::INVALID_PAGE_LSN) {
            
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
        read_index_metadata_page(metadata, root_page_id, next_page_id, metadata_lsn);
    }

    //WAL-enabled constructor
    IndexPageManager::IndexPageManager(DiskManager &disk, wal::LogManager &log_manager) :
                        disk(disk), root_page_id(format::INVALID_PAGE_ID), 
                        next_page_id(1), log_manager(&log_manager),
                        metadata_lsn(format::INVALID_PAGE_LSN) {
            
        //constructor does not need mutex locking because the object is not accessible to other threads
        //until constructor finishes
        using namespace format;

        //WAL pointer is available (due to log_manager initialization) before metadata is persisted

        if (disk.is_empty()) {
            //case1: Initialize metadata with an empty tree - create a new Index file and flush to disk
            persist_metadata();
            return;
        }

        //case2: Read existing metadata from disk and restore the root_page_id, 
        //next_page_id and allocation state - Existing Index file
        const auto metadata = disk.read_page(META_PAGE_ID);

        read_index_metadata_page(metadata, root_page_id, next_page_id, metadata_lsn);
    }

/*
    Create the FREE page and updated metadata image.
    Append a PAGE_IMAGE record for the new page (INDEX).
    Append a METADATA_UPDATE record for page 0 (INDEX).
    Put each assigned LSN into its corresponding page image.
    Flush WAL once, then write both pages and flush the database file.
*/
    std::uint64_t IndexPageManager::allocate_page() {

        std::lock_guard<std::mutex> lock(mtx); //locks Index Manager's mutex

        using namespace format;

        if (next_page_id == INVALID_PAGE_ID) {
            throw std::overflow_error("Index pageID space exhausted");
        }

        //write current page data and the immediately next page metadata to disk
        const std::uint64_t page_id = next_page_id; //reserve space for current 'next_page_id'
        auto page = make_data_page(page_id, PageType::FREE); //Initializes the page as FREE

        const std::uint64_t updated_next_page_id = next_page_id + 1;
        auto metadata = make_index_metadata_page(root_page_id, updated_next_page_id, metadata_lsn);

        //Log the new page and allocated metadata into two WAL records for persisting them before actual disk write
        if (log_manager != nullptr) {

            wal::LogRecord page_record, metadata_record;

            page_record.type = wal::LogRecordType::PAGE_IMAGE;
            page_record.page_id = page_id;
            page_record.page_space = wal::PageSpace::INDEX;
            page_record.payload.assign(page.begin(), page.end());
            const auto page_lsn = log_manager->append(page_record);
            write_page_lsn(page, page_lsn);

            metadata_record.type = wal::LogRecordType::METADATA_UPDATE;
            metadata_record.page_id = META_PAGE_ID; //always
            metadata_record.page_space = wal::PageSpace::INDEX;
            metadata_record.payload.assign(metadata.begin(), metadata.end());
            const auto new_metadata_lsn = log_manager->append(metadata_record);
            metadata = make_index_metadata_page(root_page_id, updated_next_page_id, new_metadata_lsn);

            //make both WAL records durable
            log_manager->flush();

            next_page_id = updated_next_page_id; //update counter only after WAL flush succeeds
            metadata_lsn = new_metadata_lsn;
        }

        disk.write_page(page_id, page);
        disk.write_page(META_PAGE_ID, metadata);    //write updated index metadata to page_0

        disk.flush(); //flushes the file to disk

        if (log_manager == nullptr) {   //for WAL-disabled path, update counter only after successful disk write
            next_page_id = updated_next_page_id;
        } 

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
        const auto previous_metadata_lsn = metadata_lsn;
        
        root_page_id = page_id; //asign and attempt to persist
        try {
            persist_metadata();
        } catch (...) {
            
            if (metadata_lsn == previous_metadata_lsn) {    
                //metadata_lsn was not made durable, hence rollback root_page_id
                root_page_id = previous_root_page_id;
            }
            //Do not roll back if the metadata WAL record was made durable

            throw;  //persistence failed
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
            throw std::out_of_range("Invalid Index pageID");
        }

        validate_data_page(page, page_id);  
        //validate pageID, pageType, formatVersion before WAL append,
        //since WAL should contain only valid page images.

        auto page_to_write = page;

        //WAL-aware record validation and persistence before data page is written
        if (log_manager != nullptr) {
            wal::LogRecord record;

            record.type = wal::LogRecordType::PAGE_IMAGE;
            record.page_id = page_id;
            record.page_space = wal::PageSpace::INDEX;
            record.payload.assign(page.begin(), page.end());

            const auto lsn = log_manager->append(record);
            write_page_lsn(page_to_write, lsn); //page lsn assigned after WAL append

            //WAL must be made durable before writing the actual page
            log_manager->flush();
        }

        //make the actual page durable
        disk.write_page(page_id, page_to_write);
        disk.flush();
    }

    void IndexPageManager::persist_metadata() {

        //No need to add lock_guard<mutex>, already acquired by its callers

        //serialize root_page_id and next_page_id into an index_metadata page, without actual WAL lsn
        //thie metadata is used to construct WAL record's payload
        auto metadata = format::make_index_metadata_page(root_page_id, next_page_id, metadata_lsn);

        //WAL-aware record validation and persistence before writing data to disk
        if (log_manager != nullptr) {
            wal::LogRecord record;

            record.type = wal::LogRecordType::METADATA_UPDATE;
            record.page_id = format::META_PAGE_ID;
            record.page_space = wal::PageSpace::INDEX;
            record.payload.assign(metadata.begin(), metadata.end());

            const auto lsn = log_manager->append(record);

            //rebuild index_metadata page with the assigned lsn
            metadata = format::make_index_metadata_page(root_page_id, next_page_id, lsn);

            log_manager->flush();   //make WAL durable

            metadata_lsn = lsn; //update metadata_lsn after successful WAL write
        }

        //write that index_metadata page to page_0 of the index file
        disk.write_page(format::META_PAGE_ID, metadata);

        //flush the file to underlying storage layer
        disk.flush();
    }

    //Recovery-only operations: bypass normal WAL logging

    //reads current pageLSN, returns INVALID_PAGE_LSN when the page is beyond current allocation boundary
    std::uint64_t IndexPageManager::get_page_lsn_for_recovery(std::uint64_t page_id) {

        std::lock_guard<std::mutex> lock(mtx);

        using namespace format;

        if (page_id == META_PAGE_ID) {
            throw std::invalid_argument("Metadata page does not use the data-page LSN");
        }

        //page must be present in WAL but not yet allocated on disk
        if (page_id >= next_page_id) {
            return INVALID_PAGE_LSN;
        }

        const auto page = disk.read_page(page_id);
        validate_data_page(page, page_id);

        return read_page_lsn(page);
    }

    //writes a WAL-recovered page directly, bypasses normal allocation checks and WAL logging
    void IndexPageManager::write_page_for_recovery(std::uint64_t page_id, const DiskManager::Page &page) {
        
        std::lock_guard<std::mutex> lock(mtx);

        using namespace format;

        if (page_id == META_PAGE_ID) {
            throw std::invalid_argument("Cannot write Metadata using data-page recovery method");
        }

        validate_data_page(page, page_id);

        //deliberately bypass allocation checks and WAL append
        disk.write_page(page_id, page);
        disk.flush();
    }

    //persists WAL-recovered metadata and restores next_page_id and metadata_lsn in-memory
    void IndexPageManager::restore_metadata_for_recovery(const DiskManager::Page &metadata) {

        std::lock_guard<std::mutex> lock(mtx);

        using namespace format;

        std::uint64_t recovered_next_page_id, recovered_root_page_id, recoved_metadata_lsn;
        read_index_metadata_page(metadata, recovered_root_page_id, 
            recovered_next_page_id, recoved_metadata_lsn);

        if (recoved_metadata_lsn == INVALID_PAGE_LSN) {
            throw std::runtime_error("Invalid recovered index metadata LSN");
        }

        //skip recovery if persisted metadata is already at this LSN or newer
        const auto curr_metadata = disk.read_page(META_PAGE_ID);
        std::uint64_t curr_next_page_id, curr_root_page_id, curr_metadata_lsn;
        read_index_metadata_page(curr_metadata, curr_root_page_id, 
                                curr_next_page_id, curr_metadata_lsn);

        if (curr_metadata_lsn != INVALID_PAGE_LSN && curr_metadata_lsn >= recoved_metadata_lsn) {
            return;
        }

        //Persist the recovered metadata first
        disk.write_page(META_PAGE_ID, metadata);
        disk.flush();

        //update in-memory state only after successful disk persistence
        root_page_id = recovered_root_page_id;
        next_page_id = recovered_next_page_id;
        metadata_lsn = recoved_metadata_lsn;
    }
    

} //namespace metadata::storage