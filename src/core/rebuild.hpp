#pragma once

#include <string>
#include <unordered_map>
#include <memory>
#include <atomic>
#include <mutex>
#include <filesystem>

#include "settings.hpp"
#include "log.hpp"

struct ActiveRebuild {
    std::string index_id;
    std::atomic<size_t> vectors_processed{0};
    std::atomic<size_t> total_vectors{0};
};

class Rebuild {
private:
    // Keyed by username — one rebuild per user at a time
    std::unordered_map<std::string, std::shared_ptr<ActiveRebuild>> active_rebuilds_;
    mutable std::mutex rebuild_state_mutex_;

public:
    Rebuild() = default;

    // Lifecycle — cleanup temp files from interrupted rebuilds on startup
    void cleanupTempFiles(const std::string& data_dir) {
        if (!std::filesystem::exists(data_dir)) {
            return;
        }
        try {
            std::string temp_filename = std::string(settings::DEFAULT_SUBINDEX) + ".idx.temp";
            for (const auto& entry : std::filesystem::recursive_directory_iterator(data_dir)) {
                if (entry.is_regular_file() &&
                    entry.path().filename().string() == temp_filename) {
                    std::filesystem::remove(entry.path());
                }
            }
        } catch (const std::exception&) {
            // Silently ignore cleanup errors on startup
        }
    }

    // State tracking — per user (same pattern as BackupStore)

    void setActiveRebuild(const std::string& username, const std::string& index_id,
                          size_t total_vectors) {
        std::lock_guard<std::mutex> lock(rebuild_state_mutex_);
        auto state = std::make_shared<ActiveRebuild>();
        state->index_id = index_id;
        state->total_vectors.store(total_vectors);
        state->vectors_processed.store(0);
        active_rebuilds_[username] = state;
    }

    void clearActiveRebuild(const std::string& username) {
        std::lock_guard<std::mutex> lock(rebuild_state_mutex_);
        active_rebuilds_.erase(username);
    }

    bool hasActiveRebuild(const std::string& username) const {
        std::lock_guard<std::mutex> lock(rebuild_state_mutex_);
        return active_rebuilds_.count(username) > 0;
    }

    std::shared_ptr<ActiveRebuild> getActiveRebuild(const std::string& username) const {
        std::lock_guard<std::mutex> lock(rebuild_state_mutex_);
        auto it = active_rebuilds_.find(username);
        if (it != active_rebuilds_.end()) {
            return it->second;
        }
        return nullptr;
    }

    // Path helpers

    static std::string getTempPath(const std::string& index_dir) {
        return index_dir + "/vectors/" + settings::DEFAULT_SUBINDEX + ".idx.temp";
    }

    static std::string getTimestampedPath(const std::string& index_dir) {
        auto ts = std::to_string(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count()
        );
        return index_dir + "/vectors/" + settings::DEFAULT_SUBINDEX + ".idx." + ts;
    }
};
