#ifndef MINIDB_STORAGE_PAGE_MANAGER_H
#define MINIDB_STORAGE_PAGE_MANAGER_H

#include <cstdint>
#include "minidb/storage/disk_manager.h"

namespace minidb::storage {
/*
1. DiskManager remains responsible for raw 4KB disk I/O
2. PageManager owns page aloocation and decides on next available page ID
3. Page_0 is remains reserved for database metadata
4. Copying PageManager is prohbited because it manages allocation state tied to a specific DB
*/
class PageManager {
    DiskManager &disk;
    std::uint64_t next_page_id;
public:
    explicit PageManager(DiskManager &disk);

    PageManager(const PageManager &) = delete;
    PageManager &operator=(const PageManager &) = delete;

    std::uint64_t allocate_page();

    void write_page(std::uint64_t page_id, const DiskManager::Page &page);

    DiskManager::Page read_page(std::uint64_t page_id);
};

} //namespace minidb::storage

#endif