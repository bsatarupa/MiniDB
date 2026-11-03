#include "minidb/storage/disk_manager.h"
#include "minidb/storage/index_page_manager.h"
#include "minidb/storage/page_format.h"

#include <cassert>
#include <cstdint>
#include <string>
#include <iostream>

using minidb::storage::IndexPageManager;
using namespace minidb::storage::format;

namespace {

    const std::string TEST_DIR = "build/";

    void remove_test_file(const std::string &filename) {
        std::remove(filename.c_str());
    }

    //Test1: A new indexFile initializes successfully
    void test_index_page_manager_initialization() {
        
        const std::string filename = TEST_DIR + "index_manager_init.db";
        remove_test_file(filename);

        {
            DiskManager disk(filename);
            IndexPageManager index_manager(disk);
        }
        std::cout << "[PASS] initialization\n";
    }

    //Test2: The first allocated page has pageID = 1
    void test_index_page_manager_first_allocation() {

        const std::string filename = TEST_DIR + "index_manager_first_allocation.db";
        remove_test_file(filename);

        DiskManager disk(filename);

        //case1: Initialize metadata with an empty B+tree (a new index file); index metadata occupies page_0 
        //case2: Read existing metadata from disk and restore
        IndexPageManager index_page_manager(disk);

        const std::uint64_t page_id = index_page_manager.allocate_page();

        //first allocated indexPage has page_id = 1
        assert(page_id == 1);

        //allocated page can be read back and validated to check if page_header is valid
        const auto page = index_page_manager.read_page(page_id);
        validate_data_page(page, page_id);
        std::cout << "[PASS] first allocation\n";
    }

    // Test 3: Page IDs increase sequentially.
    void test_index_page_manager_sequential_allocation() {

    const std::string filename = TEST_DIR + "index_manager_sequential.db";
    remove_test_file(filename);

    DiskManager disk(filename);
    IndexPageManager index_manager(disk);

    assert(index_manager.allocate_page() == 1);
    assert(index_manager.allocate_page() == 2);
    assert(index_manager.allocate_page() == 3);

    std::cout << "[PASS] sequential allocation\n";
    }

    // Test 4: A newly allocated page is initialized as FREE.
    void test_index_page_manager_free_page() {

    const std::string filename = TEST_DIR + "index_manager_free_page.db";
    remove_test_file(filename);

    DiskManager disk(filename);
    IndexPageManager index_manager(disk);

    const std::uint64_t page_id = index_manager.allocate_page();
    const auto page = index_manager.read_page(page_id);
    validate_data_page(page, page_id);

    const auto page_type = read_le<std::uint16_t>(page.data() + PAGE_TYPE_OFFSET);
    assert(page_type == static_cast<std::uint16_t>(PageType::FREE));

    std::cout << "[PASS] Free Page initialization\n";
    }

    // Test 5: Allocated pages can be written and read back
    void test_index_page_manager_write_read_round_trip() {

    const std::string filename = TEST_DIR + "index_manager_round_trip.db";
    remove_test_file(filename);

    DiskManager disk(filename);
    IndexPageManager index_manager(disk);

    const std::uint64_t page_id = index_manager.allocate_page();

    auto page = make_data_page(page_id, PageType::LSM_DATA);

    // Add a recognizable byte to the page payload (The First data byte right after pageHeader)
    page[PAGE_HEADER_SIZE] = std::byte{0x42};
    index_manager.write_page(page_id, page);

    const auto read_back = index_manager.read_page(page_id);
    assert(read_back == page);

    std::cout << "[PASS] write/read round-trip successful\n";
    }

    // Test 6: Page zero cannot be accessed as a data page.
    void test_index_page_manager_rejects_metadata_page() {
    
    const std::string filename = TEST_DIR + "index_manager_metadata_page.db";
    remove_test_file(filename);

    DiskManager disk(filename);
    IndexPageManager index_manager(disk);

    bool read_throw_exception = false;
    try {
        (void)index_manager.read_page(0);
    } catch (const std::out_of_range &) {
        read_throw_exception = true;
    }
    assert(read_throw_exception);

    //create a valid dataPage using a non-zero pageId
    const auto page_id = index_manager.allocate_page();
    const auto page = make_data_page(page_id, PageType::FREE);

    //page_0 must not be writable as a data page
    bool write_throw_exception = false;
    try {
        index_manager.write_page(0, page);
    } catch (const std::out_of_range &) {
        write_throw_exception = true;
    }
    assert(write_throw_exception);

    std::cout << "[PASS] MetadataPage Protection\n";
    }

    // Test 7: Unallocated page IDs cannot be read
    void test_index_page_manager_rejects_unallocated_read() {
    
    const std::string filename = TEST_DIR + "index_manager_invalid_read.db";
    remove_test_file(filename);

    DiskManager disk(filename);
    IndexPageManager index_manager(disk);

    bool throw_exception = false;
    try {
        (void)index_manager.read_page(1);   //page not allocated
    } catch (const std::out_of_range &) {
        throw_exception = true;
    }
    assert(throw_exception);

    std::cout << "[PASS] unallocated read rejected\n";
    }

    // Test 8: Unallocated page IDs cannot be written.
    void test_index_page_manager_rejects_unallocated_write() {
    const std::string filename = TEST_DIR + "index_manager_invalid_write.db";
    remove_test_file(filename);

    DiskManager disk(filename);
    IndexPageManager index_manager(disk);

    bool throw_exception = false;
    try {
        const auto page = make_data_page(1, PageType::FREE);
        index_manager.write_page(1, page);
    } catch (const std::out_of_range &) {
        throw_exception = true;
    }
    assert(throw_exception);

    std::cout << "[PASS] unallocated write rejected\n";
    }

    // Test 9: A page with a mismatched page ID is rejected.
    void test_index_page_manager_rejects_mismatched_page_id() {
    const std::string filename = TEST_DIR + "index_manager_mismatched_page.db";
    remove_test_file(filename);

    DiskManager disk(filename);
    IndexPageManager index_manager(disk);

    const std::uint64_t page_id = index_manager.allocate_page();

    // The page header says page ID 2, but we write to page 1.
    const auto wrong_page = make_data_page(page_id + 1, PageType::FREE);

    bool throw_exception = false;
    try {
        index_manager.write_page(page_id, wrong_page);
    } catch (const std::runtime_error &) {
        throw_exception = true;
    } catch (const std::invalid_argument &) {
        throw_exception = true;
    }
    assert(throw_exception);

    std::cout << "[PASS] mismatched page ID rejected\n";
    }

    //Test 10: Allocation state survives reopening the index file.
    void test_index_page_manager_persistence() {
    const std::string filename = TEST_DIR + "index_manager_persistence.db";
    remove_test_file(filename);

    {
        DiskManager disk(filename);
        IndexPageManager index_manager(disk);

        //first 2 allocations return pageId = 1 and pageId = 2
        assert(index_manager.allocate_page() == 1);
        assert(index_manager.allocate_page() == 2);
    }   //DiskManager and IndexPageManager objects are destroyed 

    {   //new objects reopen the same file
        DiskManager disk(filename);
        IndexPageManager index_manager(disk);

        //next allocation returns pageId = 3
        assert(index_manager.allocate_page() == 3);
    }
    //proves allocation was restored from persisted metadata
    std::cout << "[PASS] persistence across reopening\n";
    }

    // Test 11: A new index has no root page.
    void test_index_page_manager_initial_root() {

    const std::string filename = "build/index_manager_initial_root.db";

    std::remove(filename.c_str());

    DiskManager disk(filename);
    IndexPageManager index_manager(disk);

    assert(index_manager.get_root_page_id() == minidb::storage::format::INVALID_PAGE_ID);

    std::cout << "[PASS] initial root page\n";
    }

// Test 12: Set and retrieve a valid root page.
void test_index_page_manager_set_root() {

    const std::string filename = "build/index_manager_set_root.db";

    std::remove(filename.c_str());

    DiskManager disk(filename);
    IndexPageManager index_manager(disk);

    const std::uint64_t page_id = index_manager.allocate_page();

    index_manager.set_root_page_id(page_id);

    assert(index_manager.get_root_page_id() == page_id);

    std::cout << "[PASS] set and retrieve root page\n";
    }

    // Test 13: Root page survives reopening the index file.
    void test_index_page_manager_root_persistence() {

    const std::string filename = "build/index_manager_root_persistence.db";

    std::remove(filename.c_str());

    std::uint64_t expected_root_page_id = 0;

    {
        DiskManager disk(filename);
        IndexPageManager index_manager(disk);

        expected_root_page_id = index_manager.allocate_page();
        assert(expected_root_page_id == 1);

        index_manager.set_root_page_id(expected_root_page_id);
        assert(index_manager.get_root_page_id() == expected_root_page_id);
    }

    {   //reopen the same IndexFile
        DiskManager disk(filename);
        IndexPageManager index_manager(disk);

        assert(index_manager.get_root_page_id() == expected_root_page_id);
    }

    std::cout << "[PASS] root page persistence\n";
    }

    // Test 14: Reject an unallocated root page.
    void test_index_page_manager_rejects_invalid_root() {

    const std::string filename = "build/index_manager_invalid_root.db";

    std::remove(filename.c_str());

    DiskManager disk(filename);
    IndexPageManager index_manager(disk);

    bool threw = false;

    try {
        index_manager.set_root_page_id(1);
    } catch (const std::out_of_range&) {
        threw = true;
    }

    assert(threw);

    // A rejected update must not change the existing root.
    assert(index_manager.get_root_page_id() == minidb::storage::format::INVALID_PAGE_ID);

    std::cout << "[PASS] unallocated root rejected\n";
    }

    // Test 15: The root can be reset to INVALID_PAGE_ID.
    void test_index_page_manager_reset_root() {
    const std::string filename = "build/index_manager_reset_root.db";

    std::remove(filename.c_str());

    DiskManager disk(filename);
    IndexPageManager index_manager(disk);

    const std::uint64_t page_id = index_manager.allocate_page();

    index_manager.set_root_page_id(page_id);
    index_manager.set_root_page_id(minidb::storage::format::INVALID_PAGE_ID);

    assert(index_manager.get_root_page_id() == minidb::storage::format::INVALID_PAGE_ID);

    std::cout << "[PASS] reset root page\n";
    }   


} // namespace

int main() {
    test_index_page_manager_initialization();
    test_index_page_manager_first_allocation();
    test_index_page_manager_sequential_allocation();
    test_index_page_manager_free_page();
    test_index_page_manager_write_read_round_trip();
    test_index_page_manager_rejects_metadata_page();
    test_index_page_manager_rejects_unallocated_read();
    test_index_page_manager_rejects_unallocated_write();
    test_index_page_manager_rejects_mismatched_page_id();
    test_index_page_manager_persistence();
    test_index_page_manager_initial_root();
    test_index_page_manager_set_root();
    test_index_page_manager_root_persistence();
    test_index_page_manager_rejects_invalid_root();
    test_index_page_manager_reset_root();

    std::cout << "\n[PASS] All IndexPageManager tests passed.\n";

    return 0;
}