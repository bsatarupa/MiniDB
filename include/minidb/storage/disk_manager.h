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

    void flush();
};

#endif