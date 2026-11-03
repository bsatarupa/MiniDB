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
#include "minidb/storage/page_format.h"
#include "minidb/storage/wal/log_reader.h"
#include "minidb/storage/wal/log_record.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace minidb::storage::wal {

    RecoveryManager::RecoveryManager(LogReader &log_reader, storage::DatabasePageManager &page_manager) :
                                    log_reader(log_reader), page_manager(page_manager) {}

    void RecoveryManager::recover() {

        log_reader.open();  //open an existing WAL file on read path

        try {
            while (auto record = log_reader.read_next()) {  //1.read records sequentially from WAL file
                    redo_record(*record);
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

            //3. Read the currently persisted page using DatabasePageManager and fetch its lsn
            auto current_page = page_manager.read_page(record.page_id);
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
            page_manager.write_page(record.page_id, page);

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

            //3. Read the currently persisted page using DatabasePageManager and fetch its lsn
            auto current_page = page_manager.read_page(record.page_id);
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
            page_manager.write_page(record.page_id, current_page);

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
        case LogRecordType::METADATA_UPDATE:
            throw std::runtime_error("Unsupported WAL record type for Redo Recovery");

        case LogRecordType::INVALID:
        default:
            throw std::runtime_error("Invalid or Unknown WAL record type");
        }
    }

}   //namespace