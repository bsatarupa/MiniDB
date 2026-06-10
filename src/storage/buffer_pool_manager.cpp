#include "minidb/storage/buffer_pool_manager.h"
#include "minidb/storage/disk_manager.h"
#include "minidb/storage/database_page_manager.h"
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace minidb::storage {

    //------------------------------ReadPageGuard--------------------------------
    /*
    1. Add a shared page_latch to every frame. Page_latching protects that particular page content from conflicting access.
    2. Create a move-only ReadpageGuard. (delete Copy Constructor and Copy Assignment Operator to prevent duplicated owership of any resource. 
       PageGuards should be move-only, non-copyable. with a single well-defined owner at any point of time.)
    3. make fetch_page_read() pin a page before acuiring its latch. Pinning protects the frame from eviction while a caller is still using it.
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
                    //constructs page_lock and immediately acquires a shared_lock on the frame's page_latch

    ReadPageGuard::~ReadPageGuard() {
        drop();
    }

    void ReadPageGuard::drop() noexcept {
        //cleanup: release pin when guard is destroyed, no exception allowed to escape
        if (frame == nullptr)
            return;

        if (page_lock.owns_lock()) {
            page_lock.unlock();
        }

        //once page_latch is released/unlocked, unpin the frame through BufferPoolManager
        manager->unpin_page_internal(page_id);

        manager = nullptr;
        frame = nullptr;
    }

    const DiskManager::Page &ReadPageGuard::page() const { //guard already owns a shared_lock on the frame's page_latch
        return frame->data;
    }

    //Move Constructor, allows ownership transfer fromm another guard
    ReadPageGuard::ReadPageGuard(ReadPageGuard &&other) noexcept :
                    manager(other.manager), frame(other.frame), page_id(other.page_id), 
                    page_lock(std::move(other.page_lock)) { //lock ownership transferred instead of acquiring an additional shared lock

                other.manager = nullptr;
                other.frame = nullptr;
                other.page_id = 0;
    } 

    //Move Assignment operator, allows ownership transfer into an existing guard
    ReadPageGuard &ReadPageGuard::operator=(ReadPageGuard &&other) noexcept {

        if (this != &other) //prevent accidental destroying of own guard
            drop();

        manager = other.manager;
        frame = other.frame;
        page_id = other.page_id;
        page_lock = std::move(other.page_lock);

        other.manager = nullptr;
        other.frame = nullptr;
        other.page_id = 0;

        return *this;
    }

    ////constructor does not need mutex locking because the object has not been published to other threads yet
    BufferPoolManager::BufferPoolManager(DatabasePageManager &page_manager, std::size_t pool_size) :
                                        page_manager(page_manager), frames(pool_size) {

            if (pool_size == 0) {
                throw std::invalid_argument("Buffer pool size must be > 0");
            }
    }

    void BufferPoolManager::unpin_page_internal(std::uint64_t page_id) {
        
        std::lock_guard<std::mutex> lock(mtx);  //global mutex on pin_count

        auto it = page_table.find(page_id);
        if (it == page_table.end()) {
            return;
        }

        Frame &frame = frames[it->second];

        if (frame.pin_count == 0) {
            return;
        }

        --frame.pin_count;
    }

    //find_victim_frame() does not need its own lock as it is called from a public method that already holds the mutex
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
        const std::size_t frame_index = find_victim_frame();
        
        //Read into a temporary page before modifying the selected frame, prevents a failed Read from corrupting the frame
        DiskManager::Page page = page_manager.read_page(page_id);

        Frame &frame = frames[frame_index];

        //Write-back the victim frame before eviction; if its content have changed
        if (frame.is_occupied && frame.is_dirty) {
            page_manager.write_page(frame.page_id, frame.data);
        }

        //register new mapping to page_table before modifying the frame
        //If HT allocation fails, old frame and its existing mapping still remain intact
        auto [new_it, inserted] = page_table.emplace(page_id, frame_index);
        if (!inserted) {
            throw std::runtime_error("Page is already present in PageTable");
        }

        //The new page is ready to replace the victim
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

    void BufferPoolManager::unpin_page(std::uint64_t page_id, bool is_dirty) {

        std::lock_guard<std::mutex> lock(mtx);

        auto it = page_table.find(page_id);
        if (it == page_table.end()) {
            throw std::runtime_error("Cannot unpin a page that is NOT cached");
        }

        Frame &frame = frames[it->second];

        if (frame.pin_count == 0) {
            throw std::runtime_error("Cannot unpin a page with pin_count zero");
        }

        --frame.pin_count;

        if (is_dirty) //Dirty status is sticky until a successful write-back
            frame.is_dirty = true;
    }

    void BufferPoolManager::flush_page(std::uint64_t page_id) {

        std::lock_guard<std::mutex> lock(mtx);

        auto it = page_table.find(page_id);
        if (it == page_table.end()) {
            throw std::runtime_error("Cannot flush a page that is NOT cached");
        }

        Frame &frame = frames[it->second];
        if (!frame.is_dirty)    //no pending change to be persisted
            return;

        //write page back to current storage layer before clearing its dirty flags
        //this is not same as OS-level fsync(), provides NO guarantee against power loss
        page_manager.write_page(frame.page_id, frame.data);

        frame.is_dirty = false;
    }

    void BufferPoolManager::flush_all_pages() {

        std::lock_guard<std::mutex> lock(mtx);

        for (Frame &frame : frames) {

            if (!frame.is_occupied || !frame.is_dirty)
                continue;

            //write page back to current storage layer before clearing its dirty flags
            //this is not same as OS-level fsync(), provides NO guarantee against power loss
            page_manager.write_page(frame.page_id, frame.data);

            frame.is_dirty = false; 
        }
    }

} //namespace