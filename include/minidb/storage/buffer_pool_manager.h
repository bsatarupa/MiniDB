#ifndef MINIDB_STORAGE_BUFFER_POOL_MANAGER_H
#define MINIDB_STORAGE_BUFFER_POOL_MANAGER_H 

#include "minidb/storage/disk_manager.h"
#include "minidb/storage/database_page_manager.h"
#include <cstddef>
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <utility>  //provides std::move() to transfer guard ownership

#include <mutex> //global mutex to protect Bufferpool's metadata
#include <shared_mutex> //per-frame mutex to protect its page content

namespace minidb::storage {

    //Frame is a fixed-size memory slot that can hold 1 DB page
    struct Frame {
        DiskManager::Page data{};   //holds actual 4KB DB page
        std::shared_mutex page_latch;   //per-frame mutex to protect its page content even after the mutex scope {} ends

        std::uint64_t page_id{0};   //page_id of the page currently stored in the page-frame
        std::uint32_t pin_count{0}; //prevents eviction while a page is in use
        bool is_occupied{false};    //whether a frame is unused/occupied
        bool is_dirty{false};   //whether page needs to be written to disk
        std::uint64_t last_used{0}; //used for making LRU eviction decision
    };

    class BufferPoolManager;    //Forward declaration

    class ReadPageGuard {
    private:
        BufferPoolManager *manager{nullptr}; //helps the guard in returning the frame's pin_count when the guard is destroyed
        Frame *frame{nullptr};  //the frame whose contents the caller will read
        uint64_t page_id{0};    //records which page the guard owns

        std::shared_lock<std::shared_mutex> page_lock;  //shared lock on the frame's page_latch

        void drop() noexcept;   //cleanup function to release the pin when the guard is destroyed
    public:
        const DiskManager::Page &page() const;  //a const(caller can inspect page content but cannot modify them) reference to page's data

        ReadPageGuard() = default;

        ReadPageGuard(BufferPoolManager *manager, Frame *frame);

        //PageGuard should be non-copyable, move-only. Deleting Copy Constructor and 
        //Copy Assignment Operator to prevent accidental duplication of ownership.
        ReadPageGuard(const ReadPageGuard &) = delete;
        ReadPageGuard &operator=(const ReadPageGuard &) = delete;

        ReadPageGuard(ReadPageGuard &&other) noexcept; //&& indicates an rvalue reference. allowing ownership to transfer
        ReadPageGuard &operator=(ReadPageGuard &&other) noexcept;

        ~ReadPageGuard(); //when guard is destroyed, C++ automatically destroys its members and releases the shared lock it owns
    };

    class BufferPoolManager {
    private:
        DatabasePageManager &page_manager;

        std::vector<Frame> frames;

        //page_table helps in finding a cached pageID in O(1) time, without scanning every frame
        //contains pageID to respective frameIndex mapping
        std::unordered_map<std::uint64_t, std::size_t>  page_table;

        //monotonically increasing Logical Clock to track when a page was last accessed, required during LRU eviction decision
        std::uint64_t access_counter{0};

        //select frame to reuse when the pool is full
        std::size_t find_victim_frame() const;

        std::mutex mtx;

        void unpin_page_internal(std::uint64_t page_id); //internal function for releasing Pin owned by a ReadPageGuard

        friend class ReadPageGuard; //friend class can access private members of BufferPool

    public:
        //pool_size defines total buffer_pool capacity in terms of #pages; excluding metadata and container overhead
        BufferPoolManager(DatabasePageManager &page_manager, std::size_t pool_size);

        Frame &fetch_page(std::uint64_t page_id);

        void unpin_page(std::uint64_t page_id, bool is_dirty);

        void flush_page(std::uint64_t page_id);
        void flush_all_pages();

    };

} //namespace minidb::storage

#endif