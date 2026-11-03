#include "minidb/storage/database_page_manager.h"
#include "minidb/storage/disk_manager.h"
#include "minidb/storage/page_format.h"
#include "minidb/storage/wal/log_manager.h"
#include "minidb/storage/wal/log_record.h"
#include <cstdint>
#include <ios>
#include <limits>
#include <mutex>
#include <stdexcept>

//all accesses(read_page()/write_page()) to next_page_id must use the same mutex
namespace minidb::storage {

    //either initialize a new DB or restore allocation metadata from an existing DB
    DatabasePageManager::DatabasePageManager(DiskManager &disk) : disk(disk), next_page_id(1) {

        //constructor does not need mutex locking because the object has not been published to other threads yet
        using namespace format;

        if (disk.is_empty()) {
            const auto metadata = make_metadata_page(1);
            disk.write_page(0, metadata);
            disk.flush();
            return;
        }

        const auto metadata = disk.read_page(META_PAGE_ID);
        next_page_id = read_next_page_id(metadata);

        const auto max_page_id = static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max() / PAGE_SIZE);
        if (next_page_id > max_page_id) {
            throw std::runtime_error("Next pageID exceeds the file offset range");
        }
    }

    //WAL-enabled constructor: either initialize a new DB or restore allocation metadata from an existing DB
    DatabasePageManager::DatabasePageManager(DiskManager &disk, wal::LogManager &log_manager_) 
                                                : disk(disk), next_page_id(1), log_manager(&log_manager_) {

        //constructor does not need mutex locking because the object has not been published to other threads yet
        using namespace format;
        using namespace wal;

        if (disk.is_empty()) {

            //first create metadata_page with invalid_lsn
            auto metadata = make_metadata_page(1);

            //construct WAL metadata record and persist before actually writing to disk
            LogRecord record;
            record.type = LogRecordType::METADATA_UPDATE;
            record.page_id = META_PAGE_ID;
            record.page_space = PageSpace::DATABASE;
            record.payload.assign(metadata.begin(), metadata.end());
            metadata_lsn = log_manager->append(record);

            //now create metadata_page with metadata_lsn returned from append()
            metadata = make_metadata_page(1, metadata_lsn); //page_id 0 is reserved for META_PAGE_ID

            log_manager->flush();

            disk.write_page(META_PAGE_ID, metadata);
            disk.flush();
            return;
        }

        const auto metadata = disk.read_page(META_PAGE_ID);

        next_page_id = read_next_page_id(metadata);
        metadata_lsn = read_metadata_lsn(metadata);

        const auto max_page_id = static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max() / PAGE_SIZE);
        if (next_page_id > max_page_id) {
            throw std::runtime_error("Next pageID exceeds the file offset range");
        }
    }

    //allocate_page() : reserves a new pageID for upcoming write operation
    //write_page() : updates an already allocated page
    /*
    Create the FREE page and updated metadata image.
    Append a PAGE_IMAGE record for the new page (DATABASE).
    Append a METADATA_UPDATE record for page 0 (DATABASE).
    Put each assigned LSN into its corresponding page image.
    Flush WAL once, then write both pages and flush the database file.
    */
    std::uint64_t DatabasePageManager::allocate_page() {

        std::lock_guard<std::mutex> lock(mtx);  //locks PageManager's mutex

        using namespace format;
        using namespace wal;

        if (next_page_id == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("Page ID space exhausted");
        }

        const std::uint64_t page_id = next_page_id;
        //PageType::Free - yet to take any ownership as a B-tree or LSM page
        //write current page data and next page metadata to disk
        auto page = make_data_page(page_id, PageType::FREE);

        const std::uint64_t updated_next_page_id = page_id + 1;
        auto metadata = make_metadata_page(updated_next_page_id, metadata_lsn);

        //Log the new page and allocated metadata into two WAL records for persisting them before actual disk write
        if (log_manager != nullptr) {

            wal::LogRecord page_record, metadata_record;

            page_record.type = wal::LogRecordType::PAGE_IMAGE;
            page_record.page_id = page_id;
            page_record.page_space = wal::PageSpace::DATABASE;
            page_record.payload.assign(page.begin(), page.end());
            const auto page_lsn = log_manager->append(page_record);
            write_page_lsn(page, page_lsn);

            metadata_record.type = wal::LogRecordType::METADATA_UPDATE;
            metadata_record.page_id = META_PAGE_ID; //always
            metadata_record.page_space = wal::PageSpace::DATABASE;
            metadata_record.payload.assign(metadata.begin(), metadata.end());

            const auto new_metadata_lsn = log_manager->append(metadata_record);
            metadata = make_metadata_page(updated_next_page_id, new_metadata_lsn);

            //make both WAL records durable
            log_manager->flush();

            next_page_id = updated_next_page_id; //update counter only after WAL flush succeeds
            metadata_lsn = new_metadata_lsn;
        }

        disk.write_page(page_id, page);
        disk.write_page(META_PAGE_ID, metadata);

        disk.flush();   //flushes the file

        if (log_manager == nullptr) {   //for WAL-disabled path, update counter only after successful disk write
            next_page_id = updated_next_page_id;
        }

        return page_id; //pageID of allocated page
    }

    DiskManager::Page DatabasePageManager::read_page(std::uint64_t page_id) {

        std::lock_guard<std::mutex> lock(mtx);

        using namespace format;

        //next_page_id = next pageID available for Allocation, NOT a page that is already allocated
        if (page_id == META_PAGE_ID || page_id >= next_page_id) {
            throw std::out_of_range("Page ID is not allocated");
        }
    
        auto page = disk.read_page(page_id);
        validate_data_page(page, page_id); //validate pageID, pageType, formatVersion after reading
        return page;
    }

    void DatabasePageManager::write_page_without_wal(std::uint64_t page_id, const DiskManager::Page &page) {
        
        std::lock_guard<std::mutex> lock(mtx);
        
        using namespace format;

        if (page_id == META_PAGE_ID || page_id >= next_page_id) {   
            //page_id >= next_page_id checks for page allocation, which is omitted in write_page_for_recovery()
            throw std::out_of_range("Page ID is not allocated");
        }
    
        validate_data_page(page, page_id); //validate pageID, pageType, format Version before WAL writing

        disk.write_page(page_id, page);
        disk.flush();
    }

    void DatabasePageManager::write_page(std::uint64_t page_id, const DiskManager::Page &page) {

        std::lock_guard<std::mutex> lock(mtx);
        
        using namespace format;
        using namespace wal;

        if (page_id == META_PAGE_ID || page_id >= next_page_id) {
            throw std::out_of_range("Page ID is not allocated");
        }
    
        validate_data_page(page, page_id); //validate pageID, pageType, format Version before WAL writing

        auto page_to_write = page;

        if (log_manager != nullptr) {
            LogRecord record;

            record.type = LogRecordType::PAGE_IMAGE;
            record.page_id = page_id;
            record.page_space = PageSpace::DATABASE;
            record.payload.assign(page_to_write.begin(), page_to_write.end());

            const auto lsn = log_manager->append(record);

            write_page_lsn(page_to_write, lsn);

            log_manager->flush();
        }

        disk.write_page(page_id, page_to_write);
        disk.flush();
    }
    
    //Recovery-only operations: bypass normal WAL logging

    //reads current pageLSN, returns INVALID_PAGE_LSN when the page is beyond current allocation boundary
    std::uint64_t DatabasePageManager::get_page_lsn_for_recovery(std::uint64_t page_id) {

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
    void DatabasePageManager::write_page_for_recovery(std::uint64_t page_id, const DiskManager::Page &page) {
        
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
    void DatabasePageManager::restore_metadata_for_recovery(const DiskManager::Page &metadata) {

        std::lock_guard<std::mutex> lock(mtx);

        using namespace format;

        const auto recovered_next_page_id = read_next_page_id(metadata);
        const auto recoved_metadata_lsn = read_metadata_lsn(metadata);

        if (recoved_metadata_lsn == INVALID_PAGE_LSN) {
            throw std::runtime_error("Invalid recovered database metadata LSN");
        }

        //skip if the persisted metadata is already at this LSN or newer
        const auto curr_metadata = disk.read_page(META_PAGE_ID);
        const auto curr_metadata_lsn = read_metadata_lsn(curr_metadata);

        if (curr_metadata_lsn != INVALID_PAGE_LSN && curr_metadata_lsn >= recoved_metadata_lsn) {
            return;
        }

        //Persist the recovered metadata first
        disk.write_page(META_PAGE_ID, metadata);
        disk.flush();

        //update in-memory state only after successful disk persistence
        next_page_id = recovered_next_page_id;
        metadata_lsn = recoved_metadata_lsn;
    }

}