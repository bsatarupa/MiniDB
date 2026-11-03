#ifndef MINIDB_STORAGE_BUFFER_POOL_MANAGER_H
#define MINIDB_STORAGE_BUFFER_POOL_MANAGER_H 

#include "minidb/storage/disk_manager.h"
#include "minidb/storage/database_page_manager.h"
#include "minidb/storage/page_format.h"
#include <cstddef>
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <utility>  //provides std::move() to transfer guard ownership

#include <mutex> //global mutex to protect Bufferpool's metadata
#include <shared_mutex> //per-frame mutex to protect its page content

namespace minidb::storage::wal {
    // adding LogManager dependency for supporting WAL-before-data(WAL record of a modified page must be made 
    //durable[through page_lsn] before the actual page is written to disk)
    class LogManager;
}

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

    /* PageGuard is a sort of resource ownership certificate. a RAII wrapped around a pinned buffer_pool page and its latch.
    PageGuard manages 2 resources: Pin(to prevent eviction while the page is in-use) and latch/shared_lock(to prevent 
    conflicting page content modification)
    The guard is move-only as its pin and latch ownership must not be duplicated. Guard destructor releases the latch first
    and then only unpins the frame, ensuring the frame can't be reused/its page can't be evictedwhile guard still has access to it.

    [RAII(Resource Acquisition Is Initialization): acquires a resource object when initialized, 
    automatically releases the object when destroyed/gone out of scope. No chance of lock leaking 
    if you accidentally forget to unlock(). C++ destructor handles it.
    {
       ReadPageGuard guard = // acquire page
       read_page(); 
    }//guard destroyed here, resource automatically released

    The complete lifecycle:
    fetch_page(BufferpoolManager increments pin_count) -> construct ReadPageGuard(guard acquires shared page_latch) ->
    read page content(frame can't be evicted or modified by a writer) -> guard destruction(releases shared page_latch) ->
    unpin the page through BufferPoolManager
    */
    class ReadPageGuard {
    private:
        BufferPoolManager *manager{nullptr}; //helps BufferPoolManager in decreasing the frame's pin_count when the guard is destroyed,
        //BufferPoolManager must remain alive until all guards using it have been destroyed

        Frame *frame{nullptr};  //the frame where the actual page lives in-memory; whose contents the caller will read

        uint64_t page_id{0};    //page whose pin the guard owns. when guard release the pin, it should know which page to unpin

        std::shared_lock<std::shared_mutex> page_lock;  //shared lock on the frame's page_latch, per-frame mutex to protect its page content

        void drop() noexcept;   //cleanup function to release the pin when the guard is destroyed
    public:
        const DiskManager::Page &page() const;  //a const(caller can inspect page content but cannot modify them) reference to page's data

        ReadPageGuard() = default;

        ReadPageGuard(BufferPoolManager *manager, Frame *frame);

        //C++ ownership pattern used for resources that must have a single owner: Ownership can move, but can't be duplicated.
        //PageGuard should be non-copyable, move-only. Deleting Copy Constructor and Copy Assignment Operator to prevent accidental duplication of ownership.
        ReadPageGuard(const ReadPageGuard &) = delete;
        ReadPageGuard &operator=(const ReadPageGuard &) = delete;

        ReadPageGuard(ReadPageGuard &&other) noexcept; //&& indicates an rvalue reference. allowing ownership to transfer
        ReadPageGuard &operator=(ReadPageGuard &&other) noexcept;

        ~ReadPageGuard(); //when guard is destroyed, C++ automatically destroys its members and releases the shared lock it owns
    };

    class WritePageGuard {
        private:
            BufferPoolManager *manager{nullptr}; //helps BufferPoolManager in decreasing the frame's pin_count when the guard is destroyed,
            //BufferPoolManager must remain alive until all guards using it have been destroyed
    
            Frame *frame{nullptr};  //the frame where the actual page lives in-memory; whose contents the caller will write
    
            uint64_t page_id{0};    //page whose pin the guard owns. when guard release the pin, it should know which page to unpin
    
            std::unique_lock<std::shared_mutex> page_lock;  //exclusive lock on the frame's page_latch, per-frame mutex to protect its page content
            bool is_dirty{false};
    
            void drop() noexcept;   //cleanup function to release exclusive latch, unpin page with dirty status, clear WritePageGuard state
        public:
            WritePageGuard() = default;
    
            WritePageGuard(BufferPoolManager *manager, Frame *frame);
    
            //C++ ownership pattern used for resources that must have a single owner: Ownership can move, but can't be duplicated.
            //PageGuard should be non-copyable, move-only. Deleting Copy Constructor and Copy Assignment Operator to prevent accidental duplication of ownership.
            WritePageGuard(const WritePageGuard &) = delete;
            WritePageGuard &operator=(const WritePageGuard &) = delete;
    
            WritePageGuard(WritePageGuard &&other) noexcept; //&& indicates an rvalue reference. allowing ownership to transfer
            WritePageGuard &operator=(WritePageGuard &&other) noexcept;
    
            ~WritePageGuard(); //when guard is destroyed, C++ automatically destroys its members and releases the exclusive lock it owns

            DiskManager::Page &page();  //a reference to to-be-written page's data

            void mark_dirty() noexcept;
        };

    class BufferPoolManager {
    private:
        DatabasePageManager &page_manager;

        wal::LogManager &log_manager;
        //adding LogManager dependency for supporting WAL-before-data(WAL record of a modified page must be made 
        //durable[through page_lsn] before the actual page is written to disk)

        std::vector<Frame> frames;

        //page_table helps in finding a cached pageID in O(1) time, without scanning every frame
        //contains pageID to respective frameIndex mapping
        std::unordered_map<std::uint64_t, std::size_t>  page_table;

        //monotonically increasing Logical Clock to track when a page was last accessed, required during LRU eviction decision
        std::uint64_t access_counter{0};

        //select frame to reuse when the pool is full
        std::size_t find_victim_frame() const;

        std::mutex mtx; //global BufferPoolMutex on shared metadata(ex- pin_count, page_table)

        void unpin_page_internal(std::uint64_t page_id, bool is_dirty = false); //internal function for releasing Pin owned by a RD/WR PageGuard

        friend class ReadPageGuard; //friend class can access private members of BufferPool
        friend class WritePageGuard;
        
        /*
        DirtyPage lifecycle:
        clean page loaded from disk -> Page modified in-memory through WritePageGuard -> caller marks page dirty -> 
        page remains in bufferpool -> eviction/flush/checkpoint -> WAL flushed first -> dirty page written to disk ->
        page marked as clean and unlocked -> frame unpinned
        */
        void write_dirty_page(Frame &frame); //Write-back the victim frame before eviction, if its content was changed

        bool flush_page_internal(std::uint64_t page_id, bool missing_ok); 
        //lets flush_page()/flush_all_pages() tolerate a page that was evicted(by another thread)
        //after the snapshot(of all page_ids were taken)

    public:
        //a. pool_size defines total buffer_pool capacity in terms of #pages; excluding metadata and container overhead
        //b. adding LogManager dependency for supporting WAL-before-data(WAL record of a modified page must be made 
        //durable[through page_lsn] before the actual page is written to disk)
        BufferPoolManager(DatabasePageManager &page_manager, wal::LogManager &log_manager, std::size_t pool_size);

        /* 
        fetch_page_read() = 
            1. use fetch_page() to fetch the page and increment pin_count of its frame
            2. construct ReadPageGuard and acquire the shared page_latch
            3. return the guard to caller
        
        fetch_page_write() = 
            1. use fetch_page() to fetch the page and increment pin_count of its frame
            2. construct WritePageGuard and acquire the exclusive page_latch
            3. return the guard to caller
        */
        Frame &fetch_page(std::uint64_t page_id); //fetch page and pin the frame

        ReadPageGuard fetch_page_read(std::uint64_t page_id);
        WritePageGuard fetch_page_write(std::uint64_t page_id);

        /* unpin_page() responsibility:
         * Release a pin acquired through the raw fetch_page() API. If the page was modified, mark it dirty and update PageLSN when
         * a valid page_lsn is provided (to enforce WAL-before-data)
         * Synchronization:
         * Protects metadata using the BufferPoolManager global mutex. Does NOT acquire the per-frame page_latch.
         * Caller must ensure exclusive responsibility for page-content access until this function returns.
         */
        void unpin_page(std::uint64_t page_id, bool is_dirty, std::uint64_t page_lsn = format::INVALID_PAGE_LSN);

        void flush_page(std::uint64_t page_id);
        void flush_all_pages();

        void checkpoint();

    };

} //namespace minidb::storage

#endif