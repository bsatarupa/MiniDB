#ifndef DISK_MANAGER_H
#define DISK_MANAGER_H

#include <cstdint>
#include <fstream>
#include <ios>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
/*
1. DiskManager handles raw 4KB I/O RDs/WRs using std::fstream
2. owns and manages DB file handle
3. maps pageIDs to byte(file) offsets, reads and writes complete pages at specific pageIDs
4. flushes buffered output
5. does not manage page allocation or interpret page contents
5. cannot be copied because it owns a file resource
6. Disk I/O failures must be reported and handled explicitly
*/
class DiskManager {
    std::fstream file_stream_;

    //calculates page's byte position in the file
    std::streamoff page_offset(std::uint64_t page_id) const;
public:
    static constexpr std::size_t PAGE_SIZE = 4096;

    using Page = std::array<std::byte, PAGE_SIZE>;

    
    explicit DiskManager(const std::string& file_path);

    DiskManager(const DiskManager&) = delete;
    DiskManager& operator=(const DiskManager&) = delete;

    Page read_page(std::uint64_t page_id);
    void write_page(std::uint64_t page_id, const Page& bytes);

    bool is_empty();

    void flush();
};

#endif