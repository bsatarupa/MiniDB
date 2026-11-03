//LogReader: WAL Read path, LogManager: WAL Write path
#include "minidb/storage/wal/log_reader.h"
#include "minidb/storage/page_format.h"
#include "minidb/storage/wal/log_record.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unistd.h>
#include <fcntl.h>
#include <filesystem>
#include <unistd.h>
#include <cerrno>
#include <system_error>
#include <stdexcept>
#include <vector>
#include <algorithm>
#include <sys/stat.h>

namespace minidb::storage::wal {

    //constructor stores WAL filepath
    LogReader::LogReader(const std::filesystem::path &log_file) : log_file_(log_file) {}

    LogReader::~LogReader() {     //ensures file is closed safely
        try {
            close();
        } catch (...) {
            //Destructors must not throw exceptions
        }
    }

    void LogReader::open() {      //open an existing WAL on read path

        if (is_open_) {
            return;
        }

        fd_ = ::open(log_file_.c_str(), O_RDONLY);
        if (fd_ == -1) {
            throw std::system_error(errno, std::generic_category(), "Failed to open WAL file");
        }

        current_lsn_ = 0;  //byte_offset of next record to_be_read
        is_open_ = true;
        truncated_tail_ = false;
    }

    //WAL RD path:
    //WAL file -> read 48bytes fixed header -> extract record length -> read remaining payload + checksum ->
    // LogRecordSerializer::deserialize() -> Return LogRecord -> advance current_lsn
    std::optional<LogRecord> LogReader::read_next() {

        if (!is_open_) {
            throw std::runtime_error("WAL file is not open");
        }

        if (truncated_tail_) {  //record has truncated tail
            return std::nullopt;
        }

        //1. Read fixed-size record header (50 bytes)
        std::array<std::byte, LOG_RECORD_HEADER_SIZE> header{};
        
        std::size_t bytes_read = 0;     //total bytes read so far
        while (bytes_read < header.size()) {    //need while loop to take care of partial reads

            const ssize_t result = ::read(fd_, header.data() + bytes_read, header.size() - bytes_read);
            if (result == -1) { //failed read

                if (errno == EINTR) { //If system call is interrupted by a signal, retry; rather than treating as a permanent failure
                    continue;
                }

                throw std::system_error(errno, std::generic_category(), "Failed to read WAL header");
            }

            if (result == 0) { //no byte read in this iteration
                if (bytes_read != 0) {  //there exist remaining bytes yet to read but EOF occured
                    truncated_tail_ = true;
                }
                return std::nullopt;    //no next header
            }

            bytes_read += static_cast<std::size_t>(result);
        }

        //2a. Extract total record length and payload_length
        const std::uint32_t record_size = format::read_le<std::uint32_t>(header.data() + 4);    //RECORD_LENGTH_OFFSET = 4, @header
        if (record_size < LOG_RECORD_FIXED_SIZE) {
            throw std::runtime_error("Invalid WAL record length");
        }

        const std::uint32_t payload_size = format::read_le<std::uint32_t>(header.data() + 44);   //RECORD_PAYLOAD_LENGTH_OFFSET = 44, @header
        if (payload_size != record_size - LOG_RECORD_FIXED_SIZE) {
            throw std::runtime_error("WAL payload length mismatch");
        }

        //2b. check whether the complete record exists in file before allocating its buffer
        struct stat file_info{};
        if (::fstat(fd_, &file_info) == -1) {
            throw std::system_error(errno, std::generic_category(), "Failed to stat WAL file");
        }
        const auto file_size = static_cast<std::uint64_t>(file_info.st_size);
        if (current_lsn_ > file_size) {
            throw std::runtime_error("Invalid WAL read position");
        }
        const auto remaining_file_bytes = file_size - current_lsn_;
        if (record_size > remaining_file_bytes) {   //the record extends beyond EOF -> record got truncated tail
            truncated_tail_ = true;
            return std::nullopt;
        }

        //3. Read remaining payload + checksum to buffer
        std::vector<std::byte> buffer(record_size);
        
        std::copy(header.begin(), header.end(), buffer.begin()); //3a. copy header in buffer

        bytes_read = LOG_RECORD_HEADER_SIZE;    //initialize
        while (bytes_read < buffer.size()) {    //3b. append payload + checksum to buffer

            const ssize_t result = ::read(fd_, buffer.data() + bytes_read, buffer.size() - bytes_read);
            
            if (result == -1) { //failed read

                if (errno == EINTR) { //If system call is interrupted by a signal, retry; rather than treating as a permanent failure
                    continue;
                }

                throw std::system_error(errno, std::generic_category(), "Failed to read WAL record");
            }

            if (result == 0) {  //record is there, but could not be read completely, EOF reached
                if (bytes_read != 0) {
                    truncated_tail_ = true;
                }
                return std::nullopt;
            }

            bytes_read += static_cast<std::size_t>(result);
        }

        //4. Deserialize buffer content and validate lsn
        LogRecord record = LogRecordSerializer::deserialize(buffer);
        if (record.lsn != current_lsn_) {
            throw std::runtime_error("WAL record LSN mismatch");
        }

        //5. advance current_lsn_ after a successful record read and return LogRecord
        current_lsn_ += record_size;

        return record;
    }

    void LogReader::close() {     //close the file

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

