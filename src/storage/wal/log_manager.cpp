//LogReader: WAL Read path, LogManager: WAL Write path
#include "minidb/storage/wal/log_manager.h"
#include "minidb/storage/wal/log_record.h"
#include <cstddef>
#include <fcntl.h>
#include <filesystem>
#include <unistd.h>
#include <cerrno>
#include <system_error>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace minidb::storage::wal {

    //constructor stores WAL filepath
    LogManager::LogManager(const std::filesystem::path &log_file) : log_file_(log_file) {}

    LogManager::~LogManager() {     //ensures file is closed safely
        try {
            close();
        } catch (...) {
            //Destructors must not throw exceptions
        }
    }

    void LogManager::open() {      //open an existing WAL or create a new one

        std::lock_guard<std::mutex> lock(mtx_);

        if (is_open_) {
            return;
        }

        //need to specify mode argument (0644 here) when O_CREAT flag is used
        fd_ = ::open(log_file_.c_str(), O_CREAT | O_APPEND | O_RDWR, 0644);
        if (fd_ == -1) {
            throw std::system_error(errno, std::generic_category(), "Failed to open WAL file");
        }

        try {
            //for an existing file, recover next_lsn_ while reopening it; 
            //otherwise we end up overwriting data by considering next_lsn_{0}
            next_lsn_ = std::filesystem::file_size(log_file_);  //file_size based LSN initialization after file reopening
        } catch (...) {
            ::close(fd_);
            fd_ = -1;
            throw;
        }
    
        is_open_ = true;
    }

    LSN LogManager::append(LogRecord &record){  //LSN allocation and data buffering

        std::lock_guard<std::mutex> lock(mtx_);

        if (!is_open_) {
            throw std::runtime_error("WAL file is not open");
        }
        //Write Path Steps:
        //1. assign current WAL byte_offset as the record's LSN (monotonically increasing)
        record.lsn = next_lsn_;

        //2. serialize the record into bytes, including its assigned LSN
        std::vector<std::byte> serialized = LogRecordSerializer::serialize(record);

        //prevent LSN overflow
        if (serialized.size() > std::numeric_limits<LSN>::max() - next_lsn_) {
            throw std::overflow_error("WAL LSN overflow");
        }

        //3a. append/accumulate serialized bytes to the in-memory buffer before writing to disk
        log_buffer_.insert(log_buffer_.end(), serialized.begin(), serialized.end());

        //3b. advance to next available byte_offset
        next_lsn_ += serialized.size();

        return record.lsn;  //current record's start byte_offset

        //4. flush() in-memory WAL buffer to WAL file
        //5. clear WAL buffer after a successful flush
    }

    //flush(): write(fd_, ...) -> handle partial WRs and EINTR error -> fsync(fd_) -> clear buffer after successful sync
    void LogManager::flush() {  //durable synchronization

        std::lock_guard<std::mutex> lock(mtx_);

        if (!is_open_ || fd_ == -1) {
            throw std::runtime_error("WAL file is not open");
        }

        if (log_buffer_.empty()) {
            return;
        }
        
        //4. Transfer accumulated in-memory WAL buffer to the WAL file
        //4a. Write WAL buffered bytes to file
        while (write_offset_ < log_buffer_.size()) {    //need while loop to take care of partial writes

            const ssize_t bytes_written = ::write(fd_, log_buffer_.data() + write_offset_, log_buffer_.size() - write_offset_);

            if (bytes_written == -1) { //failed write

                if (errno == EINTR) { //If system call is interrupted by a signal, retry; rather than treating as a permanent failure
                    continue;
                }

                throw std::system_error(errno, std::generic_category(), "Failed to write WAL buffer");
            }

            if (bytes_written == 0) { //no byte read

                throw std::runtime_error("WAL write made no progress");
            }

            write_offset_ += static_cast<std::size_t>(bytes_written);
        }

        //4b. Push stream buffer to disk
        if (::fsync(fd_) == -1) {
            //If fsync() fails, write_offset_ remains at the buffer end, next flush() retries fsync() without writing the bytes again
            //in subsequent flush() retries, as above while loop will be skipped
            throw std::system_error(errno, std::generic_category(), "Failed to synchronize WAL file");
        }

        //5.clear WAL buffer and write_offset_ after a successful sync
        log_buffer_.clear(); 
        write_offset_ = 0;
    }

    LSN LogManager::get_next_lsn() const {

        std::lock_guard<std::mutex> lock(mtx_);  //to protect shared variable next_lsn_
        return next_lsn_;
    }

    void LogManager::close() {     //close the file

        std::lock_guard<std::mutex> lock(mtx_);

        if (!is_open_) {
            return;
        }

        if (::close(fd_) == -1) {   //returns a success/failure integer, not a file descriptor
            throw std::system_error(errno, std::generic_category(), "Failed to close WAL file");
        }

        fd_ = -1;
        is_open_ = false;
    }

}  //namespace

