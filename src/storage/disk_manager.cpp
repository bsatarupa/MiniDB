#include "minidb/storage/disk_manager.h"
#include <cstdint>
#include <ios>
#include <limits>
#include <stdexcept>
#include <fstream>

DiskManager::DiskManager(const std::string& file_path) {

    //Try to open the DB file for both reading and writing
    file_stream_.open(file_path, std::ios::in | std::ios::out | std::ios::binary);
    if (!file_stream_.is_open()) {
        file_stream_.clear(); //clear the stream's error flags, if opening fails

        //Try to create the file using a temporary output stream, if it does not exist
        std::ofstream create_file(file_path, std::ios::out | std::ios::binary);
        if (!create_file) {
            throw std::runtime_error("Failed to create file: " + file_path);
        }

        //closethe temporary output stream
        create_file.close();

        //reopen the file for both reading and writing
        file_stream_.open(file_path, std::ios::in | std::ios::out | std::ios::binary);
        //if reopening fails, throw an exception
        if (!file_stream_.is_open()) {
            throw std::runtime_error("Failed to open file: " + file_path);
        }
    }
}

//convert page id to byte offset in the file
std::streamoff DiskManager::page_offset(std::uint64_t page_id) const {

    //largest possible offset representable by streamoff
    const auto max_offset = std::numeric_limits<std::streamoff>::max();
    if (page_id > static_cast<std::uint64_t>(max_offset) / PAGE_SIZE) {
        throw std::out_of_range("Page ID is too large");
    }

    return static_cast<std::streamoff>(page_id * PAGE_SIZE);
}

void DiskManager::write_page(std::uint64_t page_id, const Page& bytes) {

    //clear any previous error flags on the stream
    file_stream_.clear();

    file_stream_.seekp(page_offset(page_id), std::ios::beg);
    if (!file_stream_) {
        throw std::runtime_error("Failed to seek before writing page");
    }

    file_stream_.write(reinterpret_cast<const char *>(bytes.data()),
                        static_cast<std::streamsize>(PAGE_SIZE));

    if (!file_stream_) {
        throw std::runtime_error("Failed to write database page");
    }
}

void DiskManager::flush() {

    //ask the stream to push buffered output to underlying file-writing system
    //no guarantee that it will reach a durable physical storage; use fsync()
    file_stream_.flush();
    if (!file_stream_) {
        throw std::runtime_error("Failed to flush database file");
    }
}

DiskManager::Page DiskManager::read_page(std::uint64_t page_id) {

    file_stream_.clear();

    file_stream_.seekg(page_offset(page_id), std::ios::beg);
    if (!file_stream_) {
        throw std::runtime_error("Failed to seek before reading page");
    }

    //create a 4096-bytes page buffer to read file content
    Page bytes{};
    file_stream_.read(reinterpret_cast<char *>(bytes.data()),
                static_cast<std::streamsize>(PAGE_SIZE));

    if (file_stream_.gcount() != static_cast<std::streamsize>(PAGE_SIZE)) {
        throw std::runtime_error("Could not read a complete database page"); 
    }

    return bytes;
}