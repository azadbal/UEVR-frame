#pragma once

#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>

#include <windows.h>

namespace utility {

struct SessionLogTarget {
    std::filesystem::path path;
    bool truncate{true};
    bool archive_failed{false};
    std::string archive_error{};
};

inline std::string session_log_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() % 1000;

    std::tm local_time{};
    localtime_s(&local_time, &time);

    std::ostringstream result;
    result << std::put_time(&local_time, "%Y%m%d-%H%M%S")
           << '-' << std::setw(3) << std::setfill('0') << milliseconds;
    return result.str();
}

inline bool move_session_log(const std::filesystem::path& current_log,
                             const std::filesystem::path& archive_path) {
    return MoveFileW(current_log.c_str(), archive_path.c_str()) != 0;
}

inline SessionLogTarget session_log_target(const std::filesystem::path& persistent_dir,
                                           const std::string& timestamp = {}) {
    const auto current_log = persistent_dir / "log.txt";
    std::error_code error;
    const auto current_exists = std::filesystem::exists(current_log, error);

    if (error) {
        return { current_log, false, true, "could not inspect the existing log.txt" };
    }

    if (!current_exists) {
        return { current_log, true, false, {} };
    }

    const auto logs_dir = persistent_dir / "logs";
    std::filesystem::create_directories(logs_dir, error);
    if (error) {
        return { current_log, false, true, "could not create the logs directory" };
    }

    const auto stamp = timestamp.empty() ? session_log_timestamp() : timestamp;
    for (unsigned int suffix = 0; suffix < 10000; ++suffix) {
        auto archive_name = "log-" + stamp;
        if (suffix != 0) {
            archive_name += "-" + std::to_string(suffix);
        }
        const auto archive_path = logs_dir / (archive_name + ".txt");

        if (move_session_log(current_log, archive_path)) {
            return { current_log, true, false, {} };
        }

        // MoveFileW does not replace an existing destination. Continue to a
        // fresh suffix when the timestamped name was already taken.
        std::error_code archive_exists_error;
        if (std::filesystem::exists(archive_path, archive_exists_error) && !archive_exists_error) {
            continue;
        }

        return { current_log, false, true, "could not archive the existing log.txt" };
    }

    return { current_log, false, true, "could not find a unique archive name" };
}

} // namespace utility
