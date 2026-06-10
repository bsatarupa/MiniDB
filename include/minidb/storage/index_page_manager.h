#ifndef MINIDB_STORAGE_INDEX_PAGE_MANAGER_H
#define MINIDB_STORAGE_INDEX_PAGE_MANAGER_H

#include <cstdint>
#include <mutex>

#include "minidb/storage/disk_manager.h"

namespace minidb::storage {
/*
IndexPageManager manages allocation state for a specific index file(which is a B++tree in nature)
1. DiskManager remains responsible for raw 4KB disk I/O
2. IndexPageManager owns index-page allocation and decides on next available page ID
3. Page_0 is remains reserved for index metadata, including root_page_id and next available_page_id
4. Copying IndexPageManager is prohbited because it manages allocation state tied to a specific DB
*/
class IndexPageManager {

private:
    DiskManager &disk;      //reuses existing I/O component
    std::uint64_t root_page_id; //tracks root of 'this' B+tree index, including empty-tree state
    std::uint64_t next_page_id; //tracks next available index page id
    
    mutable std::mutex mtx; 
    //protects allocation and metadata state, mutable -> to use inside const functions like get_root_page_id() 

    void persist_metadata();    //persist changes to root_page_id and next_page_id

public:
    explicit IndexPageManager(DiskManager &disk);

    //Copy constructor and copy assignment operator are deleted to prevent accidental duplicated ownership
    IndexPageManager(const IndexPageManager &) = delete;
    IndexPageManager &operator=(const IndexPageManager &) = delete;

    std::uint64_t allocate_page();

    //To update B+Tree root pageID and persist that change
    std::uint64_t get_root_page_id() const;
    void set_root_page_id(std::uint64_t page_id); 

    DiskManager::Page read_page(std::uint64_t page_id);
    void write_page(std::uint64_t page_id, const DiskManager::Page &page);
};

} //namespace minidb::storage

#endif