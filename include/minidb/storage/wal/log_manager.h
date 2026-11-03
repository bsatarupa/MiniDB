//LogReader: WAL Read path, LogManager: WAL Write path
#ifndef MINIDB_STORAGE_WAL_LOG_MANAGER_H
#define MINIDB_STORAGE_WAL_LOG_MANAGER_H

#include "minidb/storage/wal/log_record.h"

#include <cstddef>
#include <filesystem>
#include <mutex>
#include <vector>

//WAL WR path: 
// append(record) -> assign LSN -> serialize LogRecord -> append bytes to In-memory WAL buffer -> flush() -> 
// write(fd_, ) to WAL file -> fsync(fd_) -> clear WAL buffer

namespace minidb::storage::wal {

    //LogManager manages WAL file lifecycle and ownership: WAL writing, LSN allocation, buffering, and flushing
    class LogManager {
    public:
        explicit LogManager(const std::filesystem::path &log_file);

        ~LogManager();

        LogManager(const LogManager &) = delete;
        LogManager &operator=(const LogManager &) = delete;

        void open();
        void close();

        [[nodiscard]]
        LSN append(LogRecord &record);         ////LSN allocation and buffering

        void flush();   //pushes/synchronizes stream-buffered data to durable storage

        [[nodiscard]]
        LSN get_next_lsn() const;

    private:
        std::filesystem::path log_file_;    //portable filesystem path

        int fd_{-1};     //WAL file descriptor to support file read/append write

        std::vector<std::byte> log_buffer_;

        mutable std::mutex mtx_;   //to protect file operations and LSN assignment

        LSN next_lsn_{0};   //to prevent overwriting existing records
        bool is_open_{false};

        std::size_t write_offset_{0};   // #bytes written so far
    };
}   //namespace

#endif