#include "minidb/storage/disk_manager.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <iostream>
#include <string>

//namespace keeps test helpers and test_file private to this source file
namespace {

    const std::filesystem::path test_file = "build/test_data/disk_manager_test.db";

    //Testcase1: check for basic DiskManager functionalities: read_page(), write_page(), flush()
    void test_read_write_round_trip(DiskManager &disk) {
        
        DiskManager::Page expected{};

        expected[0] = std::byte{0x42};
        expected[100] = std::byte{0x7F};
        expected[DiskManager::PAGE_SIZE - 1] = std::byte{0x55};

        disk.write_page(1, expected);
        disk.flush();

        const auto actual = disk.read_page(1);
        if (actual != expected) {
            throw std::runtime_error("Page content differ after write/read");
        }

        std::cout << "[PASS] DiskManager write/read/flush round-trip\n";
    }

    //Testcase2: check for multiple non-overlapping pages
    void test_multiple_pages(DiskManager &disk) {
        
            DiskManager::Page page_a{}, page_b{};

            page_a[0] = std::byte{0x11};
            page_b[0] = std::byte{0x22};

            disk.write_page(0, page_a);
            disk.write_page(2, page_b);

            disk.flush();

            if (disk.read_page(0) != page_a) {
                throw std::runtime_error("Page_0 contents are incorrect");
            }

            if (disk.read_page(2) != page_b) {
                throw std::runtime_error("Page_2 contents are incorrect");
            }

            std::cout << "[PASS] Multiple pages remain independent\n";
    }

    //Testcase3: check for oversized page_ids
    void test_oversized_page_id(DiskManager &disk) {

         bool exception_thrown = false;

         try {
             (void)disk.read_page(std::numeric_limits<std::uint64_t>::max());
         } catch (const std::out_of_range &) {
             exception_thrown = true;
         }

         if (!exception_thrown) {    //even if unexpectedly accepted
             throw std::runtime_error("Oversized page ID did not fail as expected");
         }

         std::cout << "[PASS] Oversized page ID rejected\n";
    }

    //Testcase4: Incomplete page read that is not present on disk
    void test_incomplete_page_read(DiskManager &disk) {

            bool exception_thrown = false;

            try {
                (void)disk.read_page(10);  //a page that was never written
            } catch (const std::runtime_error &e) {

                if (std::string(e.what()) == "Could not read a complete database page") {
                    exception_thrown = true;
                } else {
                    throw;
                }
            }
                
            if (!exception_thrown) {
                    throw std::runtime_error("incomplete page read was not rejected as expected");
            }

            std::cout << "[PASS] Incomplete page read rejected\n";
    }

    //Testcase5: Overwriting an existing page
    void test_page_overwrite(DiskManager &disk) {

        DiskManager::Page original{}, updated{}, neighbour{};

        original[0] = std::byte{0x11};
        updated[0] = std::byte{0x22};
        neighbour[0] = std::byte{0x33};

        disk.write_page(1, original);
        disk.write_page(2,  neighbour);
        disk.flush();

        //Overwrite page_1
        disk.write_page(1, updated);
        disk.flush();

        if (disk.read_page(1) != updated) {
            throw std::runtime_error("Page overwrite failed");
        }

        if (disk.read_page(2) != neighbour) {
            throw std::runtime_error("Overwriting Page1 corrupted Page2");
        }

        std::cout << "[PASS] page overwrite preserves neighbouring pages\n";
    }
} //end of namespace

int main() {

    try {
        //create build/test_data directories if they don't exist; else do nothing
        std::filesystem::create_directories(test_file.parent_path());

        //remove existing test_file to create a fresh one
        std::filesystem::remove(test_file);

            {
                DiskManager disk(test_file.string());

                test_read_write_round_trip(disk);
                test_multiple_pages(disk);
                test_oversized_page_id(disk);
                test_incomplete_page_read(disk);
                test_page_overwrite(disk);
    
            }   //diskManager is destroyed and file is closed here

            std::filesystem::remove(test_file);

            std::cout << "\nAll DiskManager tests passed.\n";
            return 0;

    } catch (const std::exception &e) {

        std::cerr << "[FAIL] DiskManager tests : " << e.what() << '\n';
        return 1;
    }
}