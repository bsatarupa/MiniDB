#include "minidb/storage/buffer_pool_manager.h"
#include "minidb/storage/disk_manager.h"
#include "minidb/storage/page_format.h"
#include "minidb/storage/wal/log_manager.h"
#include "minidb/storage/wal/log_record.h"
#include "minidb/storage/database_page_manager.h"
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace minidb::storage {

    //------------------------------ReadPageGuard--------------------------------
    /*
    1. Add a shared page_latch to every frame. Page_latching protects that particular page content from conflicting access.
    2. Create a move-only ReadpageGuard. (delete Copy Constructor and Copy Assignment Operator to prevent duplicated owership of any resource. 
       PageGuards should be move-only, non-copyable. with a single well-defined owner at any point of time.)
    3. make fetch_page_read() pin a page before acquiring its latch. Pinning protects the frame from eviction while a caller is still using it.
    4. Release the latch before unpinning the page (when the guard is destroyed)
    5. The frame must remain pinned for the entire lifetime of the guard. 

    Lock Ordering:
    1. acquire the global metadata mutex
    2. find the frame and increment its pin_count
    3. release the metadata mutex
    4. acquire the specific page_latch
    5. Access the page
    6. release the page_latch
    7. Acquire the global metadata mutex and decrement frame's pin_count
    */ 
    ReadPageGuard::ReadPageGuard(BufferPoolManager *manager, Frame *frame) :
                    manager(manager), frame(frame), page_id(frame->page_id),
                    page_lock(frame->page_latch) {} 
                    //C++ initializes members in the order they are declared in the class
                    //constructs page_lock and immediately acquires a shared_lock on the frame's page_latch

                    //at this point, guard owns 1 pin on the page(acquired by the bufferPool earlier) and 
                    // 1 shared_latch on the frame -> page is protected against eviction and concurrent modification

    ReadPageGuard::~ReadPageGuard() {
        drop(); //guard releases the resources it owns
    }

    void ReadPageGuard::drop() noexcept {
        //cleanup: release pin and latch when guard is destroyed, no exception allowed to escape
        if (frame == nullptr)
            return; //an empty guard owns nothing to release

        //release/unlock page_latch, always before unpinning
        if (page_lock.owns_lock()) {
            page_lock.unlock();
        }

        //once page_latch is released/unlocked, unpin the frame through BufferPoolManager
        manager->unpin_page_internal(page_id);

        //clear the guard ownership
        manager = nullptr;
        frame = nullptr;
        page_id = 0;
    }

    //page() assumes guard is valid and guard already owns a shared_lock on the frame's page_latch
    const DiskManager::Page &ReadPageGuard::page() const { 
        return frame->data;
    }

    //Move Constructor: allows ownership transfer fromm another guard, instead of copying which may lead to incorrect pin_counts
    //new guard gets ownership of the shared_latch, old guard does not get another latch/ owns the shared_latch -> at any point in time,
    //only 1 guard owns the responsibility of releasing a specific pin and latch
    ReadPageGuard::ReadPageGuard(ReadPageGuard &&other) noexcept :
                    manager(other.manager), frame(other.frame), page_id(other.page_id), 
                    page_lock(std::move(other.page_lock)) { //lock ownership transferred instead of acquiring an additional shared lock

                other.manager = nullptr;
                other.frame = nullptr;
                other.page_id = 0;
    } 

    //Move Assignment operator : allows ownership transfer into an existing guard
    //Move Constructor always receives ownership into an empty object. But in Move Assignment, destination may already own a page,
    //so we need to be careful to avoid an overwrite.
    ReadPageGuard &ReadPageGuard::operator=(ReadPageGuard &&other) noexcept {

        if (this != &other) {
        //self-move protection against {guard1 = std::move(guard1);} - prevents accidental destroying of own-guard
        //without this check we can destroy our own resources, and then try to move from an object we just cleared
            drop();
        }

        //transfer resource (page) ownership
        manager = other.manager;
        frame = other.frame;
        page_id = other.page_id;
        page_lock = std::move(other.page_lock);

        //empty the source guard
        other.manager = nullptr;
        other.frame = nullptr;
        other.page_id = 0;

        return *this;
    }

    WritePageGuard::WritePageGuard(BufferPoolManager *manager, Frame *frame) :
                    manager(manager), frame(frame), page_id(frame->page_id),
                    page_lock(frame->page_latch) {}

    //page() assumes guard is valid and guard already owns an exclusive_lock on the frame's page_latch
    DiskManager::Page &WritePageGuard::page() { 
        return frame->data;
    }

    void WritePageGuard::mark_dirty() noexcept {    //simply records page modification status
        is_dirty = true;
    }

    //Move Constructor: allows ownership transfer fromm another guard, instead of copying which may lead to incorrect pin_counts
    //new guard gets ownership of the exclusive_latch, old guard does not get another latch/ owns the exclusive_latch -> at any point in time,
    //only 1 guard owns the responsibility of releasing a specific pin and latch
    WritePageGuard::WritePageGuard(WritePageGuard &&other) noexcept :
                    manager(other.manager), frame(other.frame), page_id(other.page_id), 
                    page_lock(std::move(other.page_lock)), //lock ownership transferred instead of acquiring an additional exclusive lock
                    is_dirty(other.is_dirty) {

                //empty the source guard
                other.manager = nullptr;
                other.frame = nullptr;
                other.page_id = 0;
                other.is_dirty = false;
    } 

    //Move Assignment operator : allows ownership transfer into an existing guard
    //Move Constructor always receives ownership into an empty object. But in Move Assignment, destination may already own a page,
    //so we need to be careful to avoid an overwrite.
    WritePageGuard &WritePageGuard::operator=(WritePageGuard &&other) noexcept {

        if (this == &other) {
            return *this;
        }
       
        //if this != &other, release pin and latch when guard is destroyed. no exception allowed to escape.
        drop();
        
        //transfer resource (page) ownership
        manager = other.manager;
        frame = other.frame;
        page_id = other.page_id;
        page_lock = std::move(other.page_lock);
        is_dirty = other.is_dirty;

        //empty the source guard
        other.manager = nullptr;
        other.frame = nullptr;
        other.page_id = 0;
        other.is_dirty = false;

        return *this;
    }

    void WritePageGuard::drop() noexcept {
        //cleanup: release pin and latch when guard is destroyed, no exception allowed to escape
        if (frame == nullptr)
            return; //an empty guard owns nothing to release

        //release/unlock page_latch, always before unpinning
        if (page_lock.owns_lock()) {
            page_lock.unlock();
        }

        //once page_latch is released/unlocked, unpin the frame through BufferPoolManager
        manager->unpin_page_internal(page_id, is_dirty);

        //clear the guard ownership
        manager = nullptr;
        frame = nullptr;
        page_id = 0;
        is_dirty = false;
    }

    WritePageGuard::~WritePageGuard() {
        drop();
    }

    //a. constructor does not need mutex locking because the object has not been published to other threads yet
    //b. adding LogManager dependency for supporting WAL-before-data(WAL record of a modified page must be made 
    //durable[through page_lsn] before the actual page is written to disk)
    BufferPoolManager::BufferPoolManager(DatabasePageManager &page_manager, wal::LogManager &log_manager, std::size_t pool_size) :
                                        page_manager(page_manager), log_manager(log_manager), frames(pool_size) {

            if (pool_size == 0) {
                throw std::invalid_argument("Buffer pool size must be > 0");
            }
    }

    void BufferPoolManager::unpin_page_internal(std::uint64_t page_id, bool is_dirty) {
        
        std::lock_guard<std::mutex> lock(mtx);  //global BufferPoolManager mutex for pin_count and page_table

        auto it = page_table.find(page_id);
        if (it == page_table.end()) {
            return;
        }

        Frame &frame = frames[it->second];

        if (frame.pin_count == 0) { //bufferPool can consider this frame for eviction
            return;
        }

        if (is_dirty) {
            frame.is_dirty = true;
        }

        frame.pin_count--;
    }

    //find_victim_frame() does not need its own lock as it is called from a public method that already holds the mutex
    //LRU eviction: selects the least recently accessed unpinned frame
    std::size_t BufferPoolManager::find_victim_frame() const {

        //prefer an unused frame
        for (std::size_t i = 0; i < frames.size(); i++) {
            if (!frames[i].is_occupied) {
                return i;
            }
        }

        //Otherwise select the LRU unpinned frame
        std::size_t victim_index = frames.size();
        std::uint64_t oldest_access = 0;

        for (std::size_t i = 0; i < frames.size(); i++) {
            const Frame &frame = frames[i];

            if (frame.pin_count > 0)    //frame currently in-use
                continue;

            if (victim_index == frames.size() ||    //BufferPool is Full, no victim chosen so far
                frame.last_used < oldest_access) {
                    victim_index = i;
                    oldest_access = frame.last_used;
            }
        }

        //no available frame, since all pages are Pinned!
        if (victim_index == frames.size()) {
            throw std::runtime_error("BufferPool has no available frame, all pages are pinned");
        }

        return victim_index;
    }

    Frame &BufferPoolManager::fetch_page(std::uint64_t page_id) {

        std::lock_guard<std::mutex> lock(mtx);

        auto it = page_table.find(page_id);

        //Case1: Cache Hit: page is already in-memory
        if (it != page_table.end()) {
         
            Frame &frame = frames[it->second];

            //check for pin_count overflow
            if (frame.pin_count == std::numeric_limits<std::uint32_t>::max()) {
                throw std::overflow_error("Page PinCount overflow");
            }

            ++frame.pin_count;
            frame.last_used = ++access_counter;

            return frame;
        }

        //Case2: Cache Miss: find an unused frame, read the page before modifying selected frame, populate frame, register to page_table
        //Serializes cache miss under the global mutex -> prevents duplicate loading of the same page
        const std::size_t frame_index = find_victim_frame();
        
        //Read into a temporary page before modifying the selected frame, prevents a failed Read from corrupting the frame
        DiskManager::Page page = page_manager.read_page(page_id);

        Frame &frame = frames[frame_index];

        //Write-back the victim frame before eviction; if its content have changed
        if (frame.is_occupied && frame.is_dirty) {
            //There are three places where dirty pages are written to disk: fetch_page(), flush_page(), flush_all_pages()
            //all three must enforce WAL-before-data by calling write_dirty_page()
            write_dirty_page(frame);
        }

        //register new mapping to page_table before modifying the frame
        //If HT allocation fails, old frame and its existing mapping still remain intact
        auto [new_it, inserted] = page_table.emplace(page_id, frame_index);
        if (!inserted) {
            throw std::runtime_error("Page is already present in PageTable");
        }

        //The new page is ready to replace the victim, if exists in page_table
        const std::uint64_t old_page_id = frame.page_id;
        const bool need_replacing_existing_page = frame.is_occupied;

        frame.data = page;
        frame.is_occupied = true;
        frame.pin_count = 1;
        frame.page_id = page_id;
        frame.is_dirty = false;
        frame.last_used = ++access_counter;

        if (need_replacing_existing_page)
            page_table.erase(old_page_id);

        return frame;
    }

    /* unpin_page() responsibility:
     * Complete the lifecycle of a raw page fetch by releasing its pin, updating WAL PageLSN when applicable, and propagating dirty state.
     * Synchronization: Uses the global metadata mutex.
     * Limitation:
     * Does not acquire the frame's page_latch. The raw API therefore requires external synchronization of page-content access.
     * Use PageGuard APIs when concurrent page access is required.
     */
    void BufferPoolManager::unpin_page(std::uint64_t page_id, bool is_dirty, std::uint64_t page_lsn) {

        std::lock_guard<std::mutex> lock(mtx);

        auto it = page_table.find(page_id);
        if (it == page_table.end()) {
            throw std::runtime_error("Cannot unpin a page that is NOT cached");
        }

        Frame &frame = frames[it->second];

        if (frame.pin_count == 0) {
            throw std::runtime_error("Cannot unpin a page with pin_count zero");
        }

        //ask is to persist WAL for the dirty page with lsn == page_lsn
        if (is_dirty && page_lsn != format::INVALID_PAGE_LSN) {

            const auto current_lsn = format::read_page_lsn(frame.data);

            if (current_lsn != format::INVALID_PAGE_LSN && page_lsn < current_lsn) {
                throw std::runtime_error("PageLSN can not move backwards");
            }

            //current_lsn <= page_lsn, enforce WAL-before-data
            //a dirty page with a valid WAL lsn(current_lsn) gets its pageLSN updated to a more recent LSN
            format::write_page_lsn(frame.data, page_lsn);
        }

        --frame.pin_count;

        if (is_dirty) //Dirty status is sticky until a successful write-back
            frame.is_dirty = true;
    }

    //There are three places where dirty pages are written to disk: fetch_page(), flush_page(), flush_all_pages()
    //all three must enforce WAL-before-data by calling write_dirty_page()
    void BufferPoolManager::write_dirty_page(Frame &frame) {

        if (!frame.is_dirty) {
            return;
        }

        //1. make WAL durable before writing corresponding data page to disk
        const auto page_lsn = format::read_page_lsn(frame.data);
        if (page_lsn != format::INVALID_PAGE_LSN) { //modified page with a valid lsn
            log_manager.flush();
        }

        //2. writing corresponding data page to disk. The page already has a WAL record, no need to append one
        page_manager.write_page_without_wal(frame.page_id, frame.data);

        //3. clear is_dirty flag only after a successful write
        frame.is_dirty = false;
    }

    bool BufferPoolManager::flush_page_internal(std::uint64_t page_id, bool missing_ok) {

        Frame *frame = nullptr;

        //Flush synchronozation and proper locking order: addresses the race between page flushing and concurrent WritePageGuard
        //1. Acquire global mutex(protects BufferPool metadata), find the frame in page_table 
        // and temporarily pin it, then release global mutex
        {
            std::lock_guard<std::mutex> lock(mtx);

            auto it = page_table.find(page_id);
            if (it == page_table.end()) {

                if (missing_ok) {
                    return false;   //unsuccessful flush
                }

                throw std::runtime_error("Cannot flush a page that is NOT cached");
            }

            frame = &frames[it->second];

            //pin_count overflow protection
            if (frame->pin_count == std::numeric_limits<std::uint32_t>::max()) {
                throw std::overflow_error("Page pin_count overflow");
            }

            frame->pin_count++;
        } //Adding a page_latch while holding global mutex can cause deadlock because PageGuard destruction
          //unlocks the page_latch before acquiring global mutex to unpin

        try {
            //2. Acquire per-frame finer lock to prevent concurrent page modifications while page flushing
            std::shared_lock<std::shared_mutex> page_lock(frame->page_latch);

            //3. Re-enter metadata synchronization(re-acquire global metadata mutex) while holding the per-frame lock
            std::lock_guard<std::mutex> lock(mtx);

            if (!frame->is_occupied || frame->page_id != page_id) {
                throw std::runtime_error("Page frame changed by another thread while flushing");
            }

            //4. Preserve WAL-before-data ordering, write dirty page back to disk while its contents are protected
            //There are three places where dirty pages are written to disk: fetch_page(), flush_page(), flush_all_pages()
            //all three must enforce WAL-before-data by calling write_dirty_page()
            write_dirty_page(*frame);

            //Release metadata lock before releasing page_latch
        } catch (...) {
            //Release temporary flush pin if anything fails
            unpin_page_internal(page_id);
            throw;
        }

        //Release temporary pin
        unpin_page_internal(page_id);

        return true; //successfully flushed
    }

    void BufferPoolManager::flush_page(std::uint64_t page_id) {
        (void)flush_page_internal(page_id, false);
    }

    void BufferPoolManager::flush_all_pages() {

        std::vector<std::uint64_t> page_ids;

        //1. Acquire global mutex, Snapshot ALL cached pageIDs under metadata synchronization, release mutex
        {
            std::lock_guard<std::mutex> lock(mtx);

            page_ids.reserve(page_table.size());

            for (const auto &[page_id, frame_index] : page_table) {
                (void)frame_index;
                page_ids.push_back(page_id);
            }
        }

        //2. Flush each page independently, flush_page_internal() handles pinning, latching, WAL ordering
        // If a page was evicted after the above snapshot, we would skip it, as eviction already writes
        // dirty pages before frame reuse.
        for (const std::uint64_t page_id : page_ids) {
            (void)flush_page_internal(page_id, true);
        }
    }

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
    ReadPageGuard BufferPoolManager::fetch_page_read(std::uint64_t page_id) {
        
        Frame &frame = fetch_page(page_id);
        try {
            return ReadPageGuard(this, &frame);
        } catch (...) {
            unpin_page_internal(page_id);   //decrement pin_count that was incremented during fetch_page()
            throw;
        }
    }
        
    WritePageGuard BufferPoolManager::fetch_page_write(std::uint64_t page_id) {

        Frame &frame = fetch_page(page_id);
        try {
            return WritePageGuard(this, &frame);
        } catch (...) {
            unpin_page_internal(page_id);   //decrement pin_count that was incremented during fetch_page()
            throw;
        }
    }

    /*
    1. Append CHECKPOINT_BEGIN and flush
    2. Flush all dirty buffer-pool pages (WAL-before-data)
    3. Append CHECKPOINT_END and flush
    4. Find latest *completed* checkpoint and redo subsequent WAL records
    */
    void BufferPoolManager::checkpoint() {

        //1. Mark the checkpoint beginning
        wal::LogRecord begin_record;
        begin_record.type = wal::LogRecordType::CHECKPOINT_BEGIN;
        (void)log_manager.append(begin_record);
        log_manager.flush();

        //2. Persist all dirty buffer-pool pages
        flush_all_pages();

        //3. Mark the checkpoint end, only after all pages are made durable
        wal::LogRecord end_record;
        end_record.type = wal::LogRecordType::CHECKPOINT_END;
        (void)log_manager.append(end_record);
        log_manager.flush();
    }

} //namespace