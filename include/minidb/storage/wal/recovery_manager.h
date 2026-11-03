#ifndef MINIDB_STORAGE_WAL_RECOVERY_MANAGER_H
#define MINIDB_STORAGE_WAL_RECOVERY_MANAGER_H

#include "minidb/storage/database_page_manager.h"
#include "minidb/storage/wal/log_reader.h"
#include "minidb/storage/wal/log_record.h"

//Redo Recovery: Replay WAL records (after a crash) using PageLSN to avoid applying an operation twice
namespace minidb::storage::wal {

    class RecoveryManager {

        private:
        LogReader &log_reader;
        ::minidb::storage::DatabasePageManager &page_manager;

        void redo_record(const LogRecord &record);

        public:
        RecoveryManager(LogReader &log_reader, storage::DatabasePageManager &page_manager);

        void recover();
    };
}   //namespace
#endif