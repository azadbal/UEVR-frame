#include "../src/utility/SessionLog.hpp"

#include <cassert>
#include <chrono>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

static void write_file(const fs::path& path, const std::string& contents) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << contents;
}

static std::string read_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return { std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{} };
}

static fs::path make_temp_directory(const std::string& prefix) {
    const auto suffix = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto base = fs::temp_directory_path() / (prefix + suffix);
    for (unsigned int attempt = 0; attempt < 100; ++attempt) {
        const auto candidate = base.string() + "-" + std::to_string(attempt);
        std::error_code error;
        if (fs::create_directory(candidate, error)) {
            return candidate;
        }
        assert(!error);
    }
    assert(false);
    return {};
}

int main() {
    const auto root = make_temp_directory("uevr-session-log-test-");

    const std::string timestamp = "20260918-010203-004";
    const auto first = utility::session_log_target(root, timestamp);
    assert(first.truncate && !first.archive_failed);
    write_file(first.path, "first session\n");

    fs::create_directories(root / "logs");
    const auto occupied = root / "logs" / ("log-" + timestamp + ".txt");
    write_file(occupied, "occupied\n");

    const auto second = utility::session_log_target(root, timestamp);
    assert(second.truncate && !second.archive_failed);
    const auto archived_first = root / "logs" / ("log-" + timestamp + "-1.txt");
    assert(read_file(archived_first) == "first session\n");
    assert(read_file(occupied) == "occupied\n");
    write_file(second.path, "second session\n");

    const auto third = utility::session_log_target(root, timestamp);
    assert(third.truncate && !third.archive_failed);
    const auto archived_second = root / "logs" / ("log-" + timestamp + "-2.txt");
    assert(read_file(archived_second) == "second session\n");

    const auto failure_root = make_temp_directory("uevr-session-log-failure-");
    write_file(failure_root / "log.txt", "previous session\n");
    write_file(failure_root / "logs", "cannot become a directory\n");

    const auto fallback = utility::session_log_target(failure_root, timestamp);
    assert(fallback.archive_failed && !fallback.truncate);
    assert(read_file(fallback.path) == "previous session\n");
    {
        std::ofstream file(fallback.path, std::ios::binary | (fallback.truncate
            ? std::ios::trunc : std::ios::app));
        file << "fallback session\n";
    }
    assert(read_file(fallback.path) == "previous session\nfallback session\n");

    fs::remove_all(root);
    fs::remove_all(failure_root);
}
