#include "minidb/storage/page_manager.h"
#include "minidb/storage/disk_manager.h"
#include "minidb/storage/page_format.h"
#include <cstdint>
#include <ios>
#include <limits>
#include <stdexcept>

namespace minidb::storage {

    //either initialize a new DB or restore allocation metadata from an existing DB
    PageManager::PageManager(DiskManager &disk) : disk(disk), next_page_id(1) {

        using namespace format;

        if (disk.is_empty()) {
            const auto metadata = make_metadata_page(1);
            disk.write_page(0, metadata);
            disk.flush();
            return;
        }

        const auto metadata = disk.read_page(META_PAGE_ID);
        next_page_id = read_next_page_id(metadata);

        const auto max_page_id = static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max() / PAGE_SIZE);
        if (next_page_id > max_page_id) {
            throw std::runtime_error("Next pageID exceeds the file offset range");
        }
    }

    //allocate_page() : reserves a new pageID for upcoming write operation
    //write_page() : updates an already allocated page
    std::uint64_t PageManager::allocate_page() {

        using namespace format;

        if (next_page_id == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("Page ID space exhausted");
        }

        const std::uint64_t page_id = next_page_id;

        //PageType::Free - yet to take any ownership as a B-tree or LSM page
        const auto page = make_data_page(page_id, PageType::FREE);
        disk.write_page(page_id, page);

        const std::uint64_t updated_next_page_id = page_id + 1;
        const auto metadata = make_metadata_page(updated_next_page_id);
        disk.write_page(META_PAGE_ID, metadata);

        disk.flush();

        //next_page_id must NOT be updated before the metadata updation succeeds.
        next_page_id = updated_next_page_id;

        return page_id;
    }

    DiskManager::Page PageManager::read_page(std::uint64_t page_id) {

        using namespace format;

        //next_page_id = next pageID available for Allocation, NOT a page that is already allocated
        if (page_id == META_PAGE_ID || page_id >= next_page_id) {
            throw std::out_of_range("Page ID is not allocated");
        }
    
        auto page = disk.read_page(page_id);
        validate_data_page(page, page_id);
        return page;
    }

    void PageManager::write_page(std::uint64_t page_id, const DiskManager::Page &page) {

        using namespace format;

        if (page_id == META_PAGE_ID || page_id >= next_page_id) {
            throw std::out_of_range("Page ID is not allocated");
        }
    
        validate_data_page(page, page_id);
        disk.write_page(page_id, page);

        disk.flush();
    } 

}