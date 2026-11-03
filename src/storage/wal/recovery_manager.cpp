/*
WAL File
    |
    v
LogReader (Read each WAL record using LogReader sequentially)
    |
    v
RecoveryManager
    |
    v
Check Record Type (For specific PageType(ex.- PAGE_IMAGE, PAGE_UPDATE), validate pageID and payload_size)
    |
    v
Read Database Page (Read the existing page using DatabasePageManager)
    |
    v
Compare Record LSN with the page's PageLSN
    |
    v
Redo Operation if Required (if record.lsn > page_lsn, copy the page image, set its page_lsn = record.lsn and persist/write it back)
    |                      (if record.lsn <= page_lsn, skip the record)
    v
Update PageLSN
    |
    v
Persist Recovered Page
*/
#include "minidb/storage/wal/recovery_manager.h"
#include "minidb/storage/database_page_manager.h"
#include "minidb/storage/disk_manager.h"
#include "minidb/storage/index_page_manager.h"
#include "minidb/storage/page_format.h"
#include "minidb/storage/wal/log_reader.h"
#include "minidb/storage/wal/log_record.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace minidb::storage::wal {

    RecoveryManager::RecoveryManager(LogReader &log_reader, ::minidb::storage::DatabasePageManager &database_page_manager,
                    ::minidb::storage::IndexPageManager &index_page_manager): log_reader(log_reader),
                    database_page_manager(database_page_manager), index_page_manager(index_page_manager) {}

    //sequence is important -> first check databasePageManager, then indexPageManager
    DiskManager::Page RecoveryManager::read_page(PageSpace page_space, std::uint64_t page_id) {

        switch (page_space) {

            case PageSpace::DATABASE:
                return database_page_manager.read_page(page_id);
            case PageSpace::INDEX:
                return index_page_manager.read_page(page_id);
            default:
                throw std::runtime_error("Invalid WAL page space");
        }
    }

    void RecoveryManager::write_page(PageSpace page_space, std::uint64_t page_id, const DiskManager::Page &page) {

        switch (page_space) {

            case PageSpace::DATABASE:
                database_page_manager.write_page_for_recovery(page_id, page);
                return;
            case PageSpace::INDEX:
                index_page_manager.write_page_for_recovery(page_id, page);
                return;
            default:
                throw std::runtime_error("Invalid WAL page space");
        }
    }

    void RecoveryManager::recover() {

        //If there exists no complete checkpoint, replay from byte offset_0
        //Replay records right after the latest *complete* CHECKPOINT_END
        LSN redo_start_lsn = 0;

        //Pass1: Find end of the latest complete checkpoint, without any redo ops
        log_reader.open();  //open the existing WAL file on read path
        try {
            bool checkpoint_started = false;

            while (auto record = log_reader.read_next()) {

                if (record->type == LogRecordType::CHECKPOINT_BEGIN) {
                    
                    checkpoint_started = true;
                }

                else if (checkpoint_started && record->type == LogRecordType::CHECKPOINT_END) {

                    redo_start_lsn = record->lsn + LOG_RECORD_FIXED_SIZE + record->payload.size();

                    checkpoint_started = false;
                }
            }

            if (log_reader.has_truncated_tail()) {  //preserve existing error behaviour
                throw std::runtime_error("Truncated WAL record");
            }

            log_reader.close(); //safely close WAL file

        } catch (...) {
            log_reader.close();
            throw;
        }

        //Pass2: restart - Find latest completed checkpoint and redo(replay) subsequent WAL records
        log_reader.open();  //open the existing WAL file on read path
        try {
            while (auto record = log_reader.read_next()) {  
                //1.read non-persisted records sequentially from WAL file

                    if (record->lsn < redo_start_lsn)   //already persisted
                        continue;
            
                    redo_record(*record);
            }

            if (log_reader.has_truncated_tail()) {
                throw std::runtime_error("Truncated WAL record");
            }

            log_reader.close(); //safely close WAL file

        } catch (...) {
            log_reader.close();
            throw;
        }
    }

    void RecoveryManager::redo_record(const LogRecord &record) {

        //reject records without an assigned lsn
        if (record.lsn == format::INVALID_PAGE_LSN) {
            throw std::runtime_error("Invalid WAL record LSN");
        }

        switch (record.type) {

        case LogRecordType::PAGE_IMAGE: //supports full-page images replay
        {
            //2. validate page_id and payload_size
            if (record.page_id == format::META_PAGE_ID) {
                throw std::runtime_error("PAGE_IMAGE can not target the metadata page");
            }

            if (record.payload.size() != format::PAGE_SIZE) {
                throw std::runtime_error("Invalid PAGE_IMAGE payload size");
            }

            PageSpace page_space = record.page_space;


            //3. Read and validate the currently persisted page using DatabasePageManager,and then fetch its lsn
            auto current_page = read_page(page_space, record.page_id);
            format::validate_data_page(current_page, record.page_id); //corrupted page format checking

            const auto current_lsn = format::read_page_lsn(current_page);

            //4. Skip the WAL record if change already applied or superseded(newer); 
            // that's why replaying the same WAL twice is idempotent
            if (current_lsn != format::INVALID_PAGE_LSN && current_lsn >= record.lsn) {
                return;
            }

            //5. Construct the new page image from WAL record(redo recovery)
            format::RawPage page{};     
            std::copy(record.payload.begin(), record.payload.end(), page.begin());
       
            format::validate_data_page(page, record.page_id);


            //6. since page_lsn < record.lsn, install WAL record's lsn into the constructed page
            format::write_page_lsn(page, record.lsn);

            //7. Persist the updated page to disk
            write_page(page_space, record.page_id, page);

            break;
        }

        case LogRecordType::PAGE_UPDATE:    //supports replay of byte_range in-place updates
        {
           //2. validate page_id and payload_size
            if (record.page_id == format::META_PAGE_ID) {
                throw std::runtime_error("PAGE_UPDATE can not target the metadata page");
            }

            //a. Use an 8-byte payload_header for WAL record: 4 bytes for update_offset inside the page 
            // and 4 bytes for updated_data_length. placed right at the beginning of record payload.
            const std::size_t UPDATE_HEADER_SIZE = 8;
            //A WAL record does not need to fit inside a 4KB database page.
            // LogManager writes serialized records to the WAL file, and records can span disk blocks or filesystem writes.
            if (record.payload.size() <= UPDATE_HEADER_SIZE) {
                throw std::runtime_error("Invalid PAGE_UPDATE payload size");
            }
        
            //b. Decode update_offset and updated_data_length (little-endian)
            const std::uint32_t update_offset = std::to_integer<std::uint8_t>(record.payload[0]) |
                                            (std::to_integer<std::uint8_t>(record.payload[1]) << 8) |
                                            (std::to_integer<std::uint8_t>(record.payload[2]) << 16) |
                                            (std::to_integer<std::uint8_t>(record.payload[3]) << 24);

            const std::uint32_t update_length = std::to_integer<std::uint8_t>(record.payload[4]) |
                                            (std::to_integer<std::uint8_t>(record.payload[5]) << 8) |
                                            (std::to_integer<std::uint8_t>(record.payload[6]) << 16) |
                                            (std::to_integer<std::uint8_t>(record.payload[7]) << 24);

            if (update_offset < format::PAGE_HEADER_SIZE ||
                update_offset > format::PAGE_SIZE ||
                update_length > format::PAGE_SIZE - update_offset) {
                    throw std::runtime_error("PAGE_UPDATE is outside page boundaries");
            }
            
            if (update_length != record.payload.size() - UPDATE_HEADER_SIZE) {
                throw std::runtime_error("PAGE_UPDATE length mismatch");
            }

            PageSpace page_space = record.page_space;
            //3. Read the currently persisted page using DatabasePageManager and fetch its lsn
            auto current_page = read_page(page_space, record.page_id);
            const auto current_lsn = format::read_page_lsn(current_page);

            //4. Skip the WAL record if change already applied or superseded(newer); 
            // that's why replaying the same WAL twice is idempotent
            if (current_lsn != format::INVALID_PAGE_LSN && current_lsn >= record.lsn) {
                return;
            }

            //5a. validate page before updating/overwriting its content
            format::validate_data_page(current_page, record.page_id);

            //5b. Update page using WAL record based on update_offset and update_length
            //an in-place overwrite of a byte_range within an existing page starting from update_offset
            std::copy(record.payload.begin() + UPDATE_HEADER_SIZE, record.payload.end(), current_page.begin() + update_offset);

            //6. since page_lsn < record.lsn, install WAL record's lsn into the constructed page
            format::write_page_lsn(current_page, record.lsn);

            //7. Persist the updated page to disk
            write_page(page_space, record.page_id, current_page);

            break;
        }
        case LogRecordType::METADATA_UPDATE:
        {
            if (record.page_id != format::META_PAGE_ID) {
                throw std::runtime_error("METADATA_UPDATE must target metadata page");
            }

            if (record.payload.size() != format::PAGE_SIZE) {
                throw std::runtime_error("Invalid METADATA_UPDATE payload size");
            }

            format::RawPage metadata{};
            std::copy(record.payload.begin(), record.payload.end(), metadata.begin());

            PageSpace page_space = record.page_space;
            switch (page_space) {
                case PageSpace::DATABASE:
                {
                    const auto next_page_id = format::read_next_page_id(metadata);

                    //rebuilding metadata from WALrecord_LSN as WAL payload was created before append() assigned the WALrecord_LSN,
                    //as a result WALpayload contains previous metadataLSN, while persisted metadata contains the new one
                    metadata = format::make_metadata_page(next_page_id, record.lsn);
                    
                    database_page_manager.restore_metadata_for_recovery(metadata);
                    break;
                }
                case PageSpace::INDEX:
                {
                    std::uint64_t root_page_id, next_page_id, metadata_lsn;

                    format::read_index_metadata_page(metadata, root_page_id, next_page_id, metadata_lsn);

                    metadata = format::make_index_metadata_page(root_page_id, next_page_id, record.lsn);

                    index_page_manager.restore_metadata_for_recovery(metadata);
                    break;
                }
                default:
                    throw std::runtime_error("Invalid METADATA_UPDATE page space");
            }

            break;
        }
        //Control records: no redo required
        case LogRecordType::BEGIN:
        case LogRecordType::COMMIT:
        case LogRecordType::ABORT:
        case LogRecordType::CHECKPOINT_BEGIN:
        case LogRecordType::CHECKPOINT_END:
            break;

        //not implemented yet
        case LogRecordType::PAGE_INSERT:
        case LogRecordType::PAGE_DELETE:
                    throw std::runtime_error("Unsupported WAL record type for Redo Recovery");

        case LogRecordType::INVALID:
        default:
            throw std::runtime_error("Invalid or Unknown WAL record type");
        }
    }

}   //namespace