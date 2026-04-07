#pragma once

#include <string>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <optional>

#include <thread>
#include <chrono>
#include <regex>
#include <mutex>

#include <archive.h>
#include <archive_entry.h>

#if defined(__unix__) || defined(__APPLE__)
#  include <fcntl.h>
#  include <unistd.h>
#  include <sys/stat.h>
#  include <cerrno>
#  include <cstring>
#endif

#include "json/nlohmann_json.hpp"
#include "index_meta.hpp"
#include "settings.hpp"
#include "log.hpp"

struct ActiveBackup {
    std::string index_id;
    std::string backup_name;
    std::jthread thread;       // jthread: built-in stop_token + auto-join on destruction
};

class BackupStore {
private:
    std::string data_dir_;
    std::unordered_map<std::string, ActiveBackup> active_user_backups_;
    mutable std::mutex active_user_backups_mutex_;

    // Writes a single file into an open libarchive writer using zero-block scanning.
    // Reads the file sequentially in 4096-byte blocks; all-zero blocks are treated
    // as holes. Non-zero data regions are registered on the PAX entry as sparse
    // extents and only their bytes are written to the archive. The PAX header stores
    // the apparent file size so restore reconstructs the original size exactly.
    //
    // IMPORTANT: archive_entry_set_size() must be called by the caller with the
    // apparent file size before invoking this function.
    bool writeSparseFileToArchive(struct archive* a,
                                   struct archive_entry* e,
                                   const std::filesystem::path& file_path,
                                   std::string& error_msg) {
#if defined(__unix__) || defined(__APPLE__)
        int fd = ::open(file_path.string().c_str(), O_RDONLY | O_CLOEXEC);
        if(fd < 0) {
            error_msg = "open() failed for " + file_path.string()
                        + ": " + std::strerror(errno);
            return false;
        }

        struct stat st;
        if(::fstat(fd, &st) < 0) {
            error_msg = std::string("fstat() failed: ") + std::strerror(errno);
            ::close(fd);
            return false;
        }
        const off_t file_size = st.st_size;

        // Empty file: write header with no data.
        if(file_size == 0) {
            if(archive_write_header(a, e) != ARCHIVE_OK) {
                error_msg = archive_error_string(a);
                ::close(fd);
                return false;
            }
            ::close(fd);
            return true;
        }

        // Zero-block scan: read sequentially in 4096-byte blocks (matches MDBX page
        // size). All-zero blocks are holes; contiguous non-zero blocks form a region.
        constexpr size_t SCAN_BLOCK = 4096;
        static const char kZeroBlock[SCAN_BLOCK] = {};

        struct SparseRegion { off_t offset; off_t length; };
        std::vector<SparseRegion> regions;

        {
            char scan_buf[SCAN_BLOCK];
            off_t off = 0;
            off_t region_start = -1;
            // fd is at offset 0 — read sequentially, no per-block lseek needed.
            while(off < file_size) {
                size_t to_read = (size_t)std::min((off_t)SCAN_BLOCK, file_size - off);
                ssize_t n = ::read(fd, scan_buf, to_read);
                if(n <= 0) break;

                bool is_zero = (memcmp(scan_buf, kZeroBlock, (size_t)n) == 0);
                if(!is_zero) {
                    if(region_start < 0) region_start = off;
                } else {
                    if(region_start >= 0) {
                        regions.push_back({region_start, off - region_start});
                        region_start = -1;
                    }
                }
                off += (off_t)n;
            }
            if(region_start >= 0) {
                regions.push_back({region_start, file_size - region_start});
            }
        }

        // Register non-zero data extents on the archive_entry.
        // archive_entry_set_size() was set to the apparent size by the caller.
        // The PAX writer auto-inserts hole records for any gaps between regions.
        if(regions.empty()) {
            // Entire file is zero. Zero-length marker tells PAX the file is sparse.
            archive_entry_sparse_add_entry(e, 0, 0);
        } else {
            for(const auto& r : regions) {
                archive_entry_sparse_add_entry(e,
                    (la_int64_t)r.offset, (la_int64_t)r.length);
            }
        }

        if(archive_write_header(a, e) != ARCHIVE_OK) {
            error_msg = archive_error_string(a);
            ::close(fd);
            return false;
        }

        // Second pass: seek to each region and write data bytes to the archive.
        // Regions are iterated from the local vector, not archive_entry_sparse_next,
        // because the libarchive sparse iterator has a use-after-free bug for single
        // full-coverage regions (sparse_reset frees the node that sparse_p points to).
        constexpr size_t BUF = 65536;
        char buf[BUF];

        for(const auto& r : regions) {
            if(::lseek(fd, r.offset, SEEK_SET) < 0) {
                error_msg = std::string("lseek failed: ") + std::strerror(errno);
                ::close(fd);
                return false;
            }
            off_t remaining = r.length;
            while(remaining > 0) {
                size_t to_read = (size_t)std::min((off_t)BUF, remaining);
                ssize_t n = ::read(fd, buf, to_read);
                if(n < 0) {
                    error_msg = std::string("read() failed: ") + std::strerror(errno);
                    ::close(fd);
                    return false;
                }
                if(n == 0) break;
                if(archive_write_data(a, buf, (size_t)n) < 0) {
                    error_msg = archive_error_string(a);
                    ::close(fd);
                    return false;
                }
                remaining -= (off_t)n;
            }
        }

        ::close(fd);
        return true;
#endif  // defined(__unix__) || defined(__APPLE__)

        // Fallback: non-POSIX platform (Windows). Sequential read, no sparse support.
        if(archive_write_header(a, e) != ARCHIVE_OK) {
            error_msg = archive_error_string(a);
            return false;
        }
        std::ifstream file(file_path, std::ios::binary);
        char buffer[8192];
        while(file.read(buffer, sizeof(buffer)) || file.gcount() > 0) {
            archive_write_data(a, buffer, file.gcount());
        }
        return true;
    }

    // Copies a single file preserving sparseness using zero-block scanning: only
    // non-zero data regions are read and written; ftruncate restores the apparent
    // size unconditionally. This prevents std::filesystem::copy from materialising
    // the full apparent size (e.g. 10+ GB) as physical disk blocks on platforms
    // where sendfile is used instead of copy_file_range.
    bool sparseCopyFile(const std::filesystem::path& src,
                        const std::filesystem::path& dst,
                        std::string& error_msg) {
#if defined(__unix__) || defined(__APPLE__)
        int src_fd = ::open(src.string().c_str(), O_RDONLY | O_CLOEXEC);
        if(src_fd < 0) {
            error_msg = "open(src) failed for " + src.string()
                        + ": " + std::strerror(errno);
            return false;
        }

        struct stat st;
        if(::fstat(src_fd, &st) < 0) {
            error_msg = std::string("fstat() failed: ") + std::strerror(errno);
            ::close(src_fd);
            return false;
        }
        const off_t file_size = st.st_size;

        int dst_fd = ::open(dst.string().c_str(),
                            O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if(dst_fd < 0) {
            error_msg = "open(dst) failed for " + dst.string()
                        + ": " + std::strerror(errno);
            ::close(src_fd);
            return false;
        }

        if(file_size == 0) {
            ::close(src_fd);
            ::close(dst_fd);
            return true;
        }

        // Zero-block scan: read src sequentially, collect non-zero data regions.
        constexpr size_t SCAN_BLOCK = 4096;
        static const char kZeroBlock[SCAN_BLOCK] = {};

        struct SparseRegion { off_t offset; off_t length; };
        std::vector<SparseRegion> regions;

        {
            char scan_buf[SCAN_BLOCK];
            off_t off = 0;
            off_t region_start = -1;
            while(off < file_size) {
                size_t to_read = (size_t)std::min((off_t)SCAN_BLOCK, file_size - off);
                ssize_t n = ::read(src_fd, scan_buf, to_read);
                if(n <= 0) break;

                bool is_zero = (memcmp(scan_buf, kZeroBlock, (size_t)n) == 0);
                if(!is_zero) {
                    if(region_start < 0) region_start = off;
                } else {
                    if(region_start >= 0) {
                        regions.push_back({region_start, off - region_start});
                        region_start = -1;
                    }
                }
                off += (off_t)n;
            }
            if(region_start >= 0) {
                regions.push_back({region_start, file_size - region_start});
            }
        }

        // Copy only the non-zero data regions, seeking src and dst to each offset.
        constexpr size_t BUF = 65536;
        char buf[BUF];

        for(const auto& r : regions) {
            if(::lseek(src_fd, r.offset, SEEK_SET) < 0 ||
               ::lseek(dst_fd, r.offset, SEEK_SET) < 0) {
                error_msg = std::string("lseek failed: ") + std::strerror(errno);
                ::close(src_fd); ::close(dst_fd);
                return false;
            }
            off_t remaining = r.length;
            while(remaining > 0) {
                size_t to_read = (size_t)std::min((off_t)BUF, remaining);
                ssize_t n = ::read(src_fd, buf, to_read);
                if(n < 0) {
                    error_msg = std::string("read() failed: ") + std::strerror(errno);
                    ::close(src_fd); ::close(dst_fd);
                    return false;
                }
                if(n == 0) break;
                ssize_t written = ::write(dst_fd, buf, (size_t)n);
                if(written != n) {
                    error_msg = std::string("write() failed: ") + std::strerror(errno);
                    ::close(src_fd); ::close(dst_fd);
                    return false;
                }
                remaining -= (off_t)n;
            }
        }

        // Always ftruncate to the apparent size: the last data region may end well
        // before file_size; without this dst would have the wrong apparent size and
        // MDBX would reject it on mmap (geometry mismatch).
        if(::ftruncate(dst_fd, file_size) < 0) {
            error_msg = std::string("ftruncate() failed: ") + std::strerror(errno);
            ::close(src_fd); ::close(dst_fd);
            return false;
        }

        ::close(src_fd);
        ::close(dst_fd);
        return true;

#else
        // Non-POSIX fallback: delegate to std::filesystem.
        std::error_code ec;
        std::filesystem::copy_file(src, dst,
            std::filesystem::copy_options::overwrite_existing, ec);
        if(ec) {
            error_msg = "copy_file failed: " + ec.message();
            return false;
        }
        return true;
#endif
    }

public:
    BackupStore(const std::string& data_dir)
        : data_dir_(data_dir) {
        std::filesystem::create_directories(data_dir + "/backups");
        cleanupTempDir();
    }

    // Recursively copies src_dir → dst_dir preserving sparseness on each file.
    // Replaces std::filesystem::copy in the restore path to avoid materialising
    // the full apparent size of sparse MDBX files as physical disk blocks.
    bool sparseCopyDirectory(const std::filesystem::path& src_dir,
                              const std::filesystem::path& dst_dir,
                              std::string& error_msg) {
        for(const auto& entry : std::filesystem::recursive_directory_iterator(src_dir)) {
            std::filesystem::path rel = std::filesystem::relative(entry.path(), src_dir);
            std::filesystem::path dst_path = dst_dir / rel;

            if(entry.is_directory()) {
                std::filesystem::create_directories(dst_path);
            } else if(entry.is_regular_file()) {
                std::filesystem::create_directories(dst_path.parent_path());
                if(!sparseCopyFile(entry.path(), dst_path, error_msg)) {
                    return false;
                }
            }
        }
        return true;
    }

    // Archive methods

    bool createBackupTar(const std::filesystem::path& source_dir,
                         const std::filesystem::path& archive_path,
                         std::string& error_msg,
                         std::stop_token st = {}) {
        struct archive* a = archive_write_new();
        archive_write_set_format_pax_restricted(a);

        if(archive_write_open_filename(a, archive_path.string().c_str()) != ARCHIVE_OK) {
            error_msg = archive_error_string(a);
            archive_write_free(a);
            return false;
        }

        for(const auto& entry : std::filesystem::recursive_directory_iterator(source_dir)) {
            // Check stop_token per-file so shutdown doesn't block on large tar operations
            if(st.stop_requested()) {
                archive_write_close(a);
                archive_write_free(a);
                error_msg = "Backup cancelled";
                return false;
            }
            if(entry.is_regular_file()) {
                struct archive_entry* e = archive_entry_new();

                std::filesystem::path rel_path =
                        std::filesystem::relative(entry.path(), source_dir.parent_path());
                archive_entry_set_pathname(e, rel_path.string().c_str());
                // Must be set to apparent size before writeSparseFileToArchive
                // calls archive_write_header internally.
                archive_entry_set_size(e, (la_int64_t)std::filesystem::file_size(entry.path()));
                archive_entry_set_filetype(e, AE_IFREG);
                archive_entry_set_perm(e, 0644);

                if(!writeSparseFileToArchive(a, e, entry.path(), error_msg)) {
                    archive_entry_free(e);
                    archive_write_close(a);
                    archive_write_free(a);
                    return false;
                }
                archive_entry_free(e);
            }
        }

        archive_write_close(a);
        archive_write_free(a);
        return true;
    }

    bool extractBackupTar(const std::filesystem::path& archive_path,
                          const std::filesystem::path& dest_dir,
                          std::string& error_msg) {
        struct archive* a = archive_read_new();
        struct archive* ext = archive_write_disk_new();
        struct archive_entry* entry;

        archive_read_support_format_all(a);
        archive_read_support_filter_all(a);
        // ARCHIVE_EXTRACT_SPARSE: tells the disk writer to create actual sparse
        // files by seeking to data offsets and leaving holes, rather than writing
        // zeros. Combined with archive_write_data_block's offset parameter and
        // finish_entry's ftruncate, this restores both apparent size and sparseness.
        archive_write_disk_set_options(ext,
            ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM | ARCHIVE_EXTRACT_SPARSE);
        archive_write_disk_set_standard_lookup(ext);

        if(archive_read_open_filename(a, archive_path.string().c_str(), 10240) != ARCHIVE_OK) {
            error_msg = archive_error_string(a);
            archive_read_free(a);
            archive_write_free(ext);
            return false;
        }

        while(archive_read_next_header(a, &entry) == ARCHIVE_OK) {
            std::filesystem::path full_path = dest_dir / archive_entry_pathname(entry);
            archive_entry_set_pathname(entry, full_path.string().c_str());

            if(archive_write_header(ext, entry) == ARCHIVE_OK) {
                const void* buff;
                size_t size;
                la_int64_t offset;

                while(archive_read_data_block(a, &buff, &size, &offset) == ARCHIVE_OK) {
                    archive_write_data_block(ext, buff, size, offset);
                }
            }
            archive_write_finish_entry(ext);
        }

        archive_read_close(a);
        archive_read_free(a);
        archive_write_close(ext);
        archive_write_free(ext);
        return true;
    }

    // Path helpers

    std::string getUserBackupDir(const std::string& username) const {
        return data_dir_ + "/backups/" + username;
    }

    std::string getBackupJsonPath(const std::string& username) const {
        return getUserBackupDir(username) + "/backup.json";
    }

    std::string getUserTempDir(const std::string& username) const {
        return data_dir_ + "/backups/.tmp/" + username;
    }

    // Backup JSON helpers

    nlohmann::json readBackupJson(const std::string& username) {
        std::string path = getBackupJsonPath(username);
        if (!std::filesystem::exists(path)) return nlohmann::json::object();
        try {
            std::ifstream f(path);
            return nlohmann::json::parse(f);
        } catch (const std::exception& e) {
            LOG_WARN(1304,
                          username,
                          "Failed to parse backup metadata file " << path << ": " << e.what());
            return nlohmann::json::object();
        }
    }

    void writeBackupJson(const std::string& username, const nlohmann::json& data) {
        std::string path = getBackupJsonPath(username);
        std::ofstream f(path);
        f << data.dump(2);
    }

    // Temp directory cleanup

    void cleanupTempDir() {
        std::string temp_dir = data_dir_ + "/backups/.tmp";
        if (std::filesystem::exists(temp_dir)) {
            try {
                std::filesystem::remove_all(temp_dir);
                LOG_INFO(1301, "Cleaned up backup temp directory");
            } catch (const std::exception& e) {
                LOG_ERROR(1302, "Failed to clean up backup temp directory: " << e.what());
            }
        }
    }

    // Active backup tracking

    void setActiveBackup(const std::string& username, const std::string& index_id,
                         const std::string& backup_name, std::jthread&& thread) {
        std::lock_guard<std::mutex> lock(active_user_backups_mutex_);
        active_user_backups_[username] = {index_id, backup_name, std::move(thread)};
    }

    void clearActiveBackup(const std::string& username) {
        std::lock_guard<std::mutex> lock(active_user_backups_mutex_);
        auto it = active_user_backups_.find(username);
        if (it != active_user_backups_.end()) {
            // Called from within the thread itself — detach so erase doesn't try to join
            if (it->second.thread.joinable()) {
                it->second.thread.detach();
            }
            active_user_backups_.erase(it);
        }
    }

    bool hasActiveBackup(const std::string& username) const {
        std::lock_guard<std::mutex> lock(active_user_backups_mutex_);
        return active_user_backups_.count(username) > 0;
    }

    // Join all background backup threads before destroying IndexManager members.
    // Moves threads out under lock, then request_stop + join outside lock to avoid
    // deadlock (finishing threads call clearActiveBackup which also locks active_user_backups_mutex_).
    void joinAllThreads() {
        std::vector<std::jthread> threads_to_join;
        {
            std::lock_guard<std::mutex> lock(active_user_backups_mutex_);
            for (auto& [username, backup] : active_user_backups_) {
                if (backup.thread.joinable()) {
                    threads_to_join.push_back(std::move(backup.thread));
                }
            }
            active_user_backups_.clear();
        }
        // request_stop + join outside the lock
        for (auto& t : threads_to_join) {
            t.request_stop();   // signal stop_token — thread sees it inside createBackupTar
            if (t.joinable()) {
                t.join();
            }
        }
    }

    // Backup name validation

    std::pair<bool, std::string> validateBackupName(const std::string& backup_name) const {
        if(backup_name.empty()) {
            return std::make_pair(false, "Backup name cannot be empty");
        }

        if(backup_name.length() > settings::MAX_BACKUP_NAME_LENGTH) {
            return std::make_pair(false,
                                  "Backup name too long (max "
                                          + std::to_string(settings::MAX_BACKUP_NAME_LENGTH)
                                          + " characters)");
        }

        static const std::regex backup_name_regex("^[a-zA-Z0-9_-]+$");
        if(!std::regex_match(backup_name, backup_name_regex)) {
            return std::make_pair(false,
                                  "Invalid backup name: only alphanumeric, underscores, "
                                  "and hyphens allowed");
        }

        return std::make_pair(true, "");
    }

    // Backup listing

    nlohmann::json listBackups(const std::string& username) {
        nlohmann::json backup_list_json = readBackupJson(username);
        return backup_list_json;
    }

    // Backup deletion

    std::pair<bool, std::string> deleteBackup(const std::string& backup_name,
                                               const std::string& username) {
        std::pair<bool, std::string> result = validateBackupName(backup_name);
        if(!result.first) {
            return result;
        }

        std::string backup_tar = getUserBackupDir(username) + "/" + backup_name + ".tar";

        if(std::filesystem::exists(backup_tar)) {
            std::filesystem::remove(backup_tar);

            nlohmann::json backup_db = readBackupJson(username);
            backup_db.erase(backup_name);
            writeBackupJson(username, backup_db);

            LOG_INFO(1303, username, "Deleted backup " << backup_tar);
            return {true, ""};
        } else {
            return {false, "Backup not found"};
        }
    }

    // Active backup query

    std::optional<std::pair<std::string, std::string>> getActiveBackup(const std::string& username) {
        std::lock_guard<std::mutex> lock(active_user_backups_mutex_);
        auto it = active_user_backups_.find(username);
        if (it != active_user_backups_.end()) {
            return std::make_pair(it->second.index_id, it->second.backup_name);
        }
        return std::nullopt;
    }

    // Backup info

    nlohmann::json getBackupInfo(const std::string& backup_name, const std::string& username) {
        nlohmann::json backup_db = readBackupJson(username);
        if (backup_db.contains(backup_name)) {
            return backup_db[backup_name];
        }
        return nlohmann::json();
    }
};
