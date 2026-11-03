#include "minidb/storage/page_format.h"
#include "minidb/storage/database_page_manager.h"
#include "minidb/storage/disk_manager.h"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <exception>
#include <limits>
#include <stdexcept>

namespace {

    void test_sequential_allocation() {
        const std::string path = "build/test_data/page_manager_sequential.db";
        std::remove(path.c_str());  //delete file if already exists at specified path

        DiskManager disk(path);
        minidb::storage::DatabasePageManager manager(disk);

        const auto first = manager.allocate_page();
        const auto second = manager.allocate_page();
        const auto third = manager.allocate_page();

        if (first != 1 || second != 2 || third != 3) {
            throw std::runtime_error("Page IDs were not allocated sequentially");
        }

        std::remove(path.c_str());
    }

    void test_allocation_persists_after_reopen() {
        const std::string path = "build/test_data/page_manager_persistence.db";
        std::remove(path.c_str());  //delete file if already exists at specified path

        {
            DiskManager disk(path);
            minidb::storage::DatabasePageManager manager(disk);

            if (manager.allocate_page() != 1) {
                throw std::runtime_error("First allocated page ID should be 1");
            }

            if (manager.allocate_page() != 2) {
                throw std::runtime_error("Second allocated page ID should be 2");
            }
        }

        //reopening DB
        {
            DiskManager disk(path);
            minidb::storage::DatabasePageManager manager(disk);

            if (manager.allocate_page() != 3) {
                throw std::runtime_error("Allocation Counter did not persist after reopening");
            }
        } 

        std::remove(path.c_str());
    }

    void test_page_read_write_round_trip() {

        using namespace minidb::storage::format;

        const std::string path = "build/test_data/page_manager_read_write.db";
        std::remove(path.c_str());  //delete file if already exists at specified path

        {
            DiskManager disk(path);
            minidb::storage::DatabasePageManager manager(disk);

            const auto page_id = manager.allocate_page();
            auto page = make_data_page(page_id, PageType::BTREE_LEAF);

            page[PAGE_HEADER_SIZE] = std::byte{0xAB};  //as 1st data payload byte
            manager.write_page(page_id, page);

            const auto loaded = manager.read_page(page_id);
            if (page != loaded) {
                throw std::runtime_error("Page read/write round trip failed");
            }
        }
        std::remove(path.c_str());
    }

    void test_unallocated_page_read_rejected() {
        const std::string path = "build/test_data/page_manager_invalid_read.db";
        std::remove(path.c_str());
    
        {
            //create a fresh DB file and PageManager initializes its allocation state
            DiskManager disk(path);
            minidb::storage::DatabasePageManager manager(disk);
    
            bool rejected = false;
            try {
                (void)manager.read_page(1);
            } catch (const std::out_of_range&) {
                rejected = true;
            }
    
            if (!rejected) {
                throw std::runtime_error("Unallocated page read was accepted");
            }
        }
    
        std::remove(path.c_str());
    }
    
    void test_corrupted_page_header_rejected() {

        using namespace minidb::storage::format;

        const std::string path = "build/test_data/page_manager_corrupt.db";
        std::remove(path.c_str());
    
        {   //create a fresh DB file and PageManager initializes its allocation state
            DiskManager disk(path);
            minidb::storage::DatabasePageManager manager(disk);
    
            const auto page_id = manager.allocate_page();
    
            //page_id is evaluated to be 1
            auto page = make_data_page(page_id, PageType::BTREE_LEAF);

            //deliberately writing wrong page_id (=2) in page header(i.e., within first 16bytes of a 4KB page)
            write_le<std::uint64_t>(page.data() + PAGE_ID_OFFSET,page_id + 1);
    
            bool rejected = false;
            try {
                manager.write_page(page_id, page);
            } catch (const std::runtime_error&) {
                rejected = true;
            }
    
            if (!rejected) {
                throw std::runtime_error("Corrupted page header was accepted");
            }
        }
        std::remove(path.c_str());
    }

    void test_truncated_database_rejected() {

        using namespace minidb::storage::format;

        const std::string path = "build/test_data/page_manager_corrupt.db";
        std::remove(path.c_str());
    
        {   //create a non-empty file, too small to contain page_0
            std::ofstream file(path, std::ios::binary);
            file.put('X');  //file size = 1 byte
        }
    
        bool rejected = false;
        try {
                DiskManager disk(path);
                minidb::storage::DatabasePageManager manager(disk); 
                //PageManager fails to read 4KB metadata page (page_0) from the existing 1byte file
                //for any new file (size = 0), PageManager first writes a 4KB metadata page; for existing files this step is skipped.
        } catch (const std::exception &) {
                rejected = true;
        }
        std::remove(path.c_str());

        if (!rejected) {
            throw std::runtime_error("Truncated database was accepted");
        }
    }

    void test_out_of_range_next_page_id_rejected() {

        using namespace minidb::storage::format;

        const std::string path = "build/test_data/page_manager_invalid_metadata.db";
        std::remove(path.c_str());
    
        {
            DiskManager disk(path);

            //write metadata page first
            auto metadata = make_metadata_page(std::numeric_limits<uint64_t>::max());
            disk.write_page(0, metadata);

            disk.flush();
        }
    
        bool rejected = false;
        try {
                DiskManager disk(path);
                minidb::storage::DatabasePageManager manager(disk); 
                
        } catch (const std::exception &) {
                rejected = true;
        }
        std::remove(path.c_str());

        if (!rejected) {
            throw std::runtime_error("Out-of-range allocation metadata was accepted");
        }
    }

} //namespace

int main() {

    try {

        test_sequential_allocation();
        std::cout << "[PASS] Sequential page allocation\n";

        test_allocation_persists_after_reopen();
        std::cout << "[PASS] Allocation persists after reopening\n";

        test_page_read_write_round_trip();
        test_corrupted_page_header_rejected();
        test_unallocated_page_read_rejected();

        test_truncated_database_rejected();
        test_out_of_range_next_page_id_rejected();

        std::cout << "\nAll Page Manager tests passed\n";
        return 0;

    } catch (const std::exception &error) {
        std::cerr << "[FAIL] " << error.what() << '\n';
        return 1;
    }
}