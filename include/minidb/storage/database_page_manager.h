#ifndef MINIDB_STORAGE_DATABASE_PAGE_MANAGER_H
#define MINIDB_STORAGE_DATABASE_PAGE_MANAGER_H

#include <cstdint>
#include <mutex>

#include "minidb/storage/disk_manager.h"
#include "minidb/storage/page_format.h"
#include "minidb/storage/wal/log_manager.h"

namespace minidb::storage {
/*
1. DiskManager remains responsible for raw 4KB disk I/O
2. DatabasePageManager owns page allocation and decides on next available page ID
3. Page_0 is remains reserved for database metadata
4. Copying PageManager is prohbited because it manages allocation state tied to a specific DB
*/
class DatabasePageManager {

private:
    DiskManager &disk;  //reuses existing I/O component
    std::uint64_t next_page_id; //tracks next available database page id
    std::mutex mtx; //protects allocation and metadata state

public:
    explicit DatabasePageManager(DiskManager &disk);

    DatabasePageManager(DiskManager &disk, wal::LogManager &log_manager);

    //Copy constructor and copy assignment operator are deleted to prevent accidental duplicated ownership
    DatabasePageManager(const DatabasePageManager &) = delete;
    DatabasePageManager &operator=(const DatabasePageManager &) = delete;

    std::uint64_t allocate_page();

    void write_page(std::uint64_t page_id, const DiskManager::Page &page);

    void write_page_without_wal(std::uint64_t page_id, const DiskManager::Page &page);

    DiskManager::Page read_page(std::uint64_t page_id);

    //WAL persistence and recovery
    wal::LogManager *log_manager{nullptr};
    std::uint64_t metadata_lsn{format::INVALID_PAGE_LSN};

    //Recovery-only operations: bypass normal WAL logging
    std::uint64_t get_page_lsn_for_recovery(std::uint64_t page_id);

    void write_page_for_recovery(std::uint64_t page_id, const DiskManager::Page &page);

    void restore_metadata_for_recovery(const DiskManager::Page &metadata);
};

} //namespace minidb::storage

#endif