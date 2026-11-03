#ifndef MINIDB_STORAGE_WAL_RECOVERY_MANAGER_H
#define MINIDB_STORAGE_WAL_RECOVERY_MANAGER_H

#include "minidb/storage/database_page_manager.h"
#include "minidb/storage/disk_manager.h"
#include "minidb/storage/index_page_manager.h"
#include "minidb/storage/wal/log_reader.h"
#include "minidb/storage/wal/log_record.h"
#include <cstdint>

//Redo Recovery: Replay WAL records (after a crash) using PageLSN to avoid applying an operation twice
namespace minidb::storage::wal {

    class RecoveryManager {

        private:
        LogReader &log_reader;
        ::minidb::storage::DatabasePageManager &database_page_manager;
        ::minidb::storage::IndexPageManager &index_page_manager;

        //shared page access for both manager types
        DiskManager::Page read_page(PageSpace page_space, std::uint64_t page_id);
        void write_page(PageSpace page_space, std::uint64_t page_id, const DiskManager::Page &page);

        void redo_record(const LogRecord &record);

        public:
        RecoveryManager(LogReader &log_reader, 
                        ::minidb::storage::DatabasePageManager &database_page_manager,
                        ::minidb::storage::IndexPageManager &index_page_manager);

        void recover();
    };
}   //namespace
#endif