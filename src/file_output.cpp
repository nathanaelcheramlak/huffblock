#include "file_output.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace huffblock::detail {
namespace {

std::filesystem::path make_directory(const std::filesystem::path& parent) {
    static std::atomic<std::uint64_t> counter{0};
    std::random_device random;
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        const auto name = ".huffblock-" + std::to_string(now) + "-" +
                          std::to_string(random()) + "-" + std::to_string(counter.fetch_add(1));
        const auto candidate = parent / name;
        std::error_code error;
        if (std::filesystem::create_directory(candidate, error)) {
#ifndef _WIN32
            std::filesystem::permissions(candidate, std::filesystem::perms::owner_all,
                                         std::filesystem::perm_options::replace, error);
            if (error) {
                std::error_code ignored;
                std::filesystem::remove(candidate, ignored);
                throw std::filesystem::filesystem_error("cannot protect temporary directory", candidate, error);
            }
#endif
            return candidate;
        }
        if (error) {
            throw std::filesystem::filesystem_error("cannot create temporary directory", candidate, error);
        }
    }
    throw std::runtime_error("cannot choose an unused temporary directory");
}

void publish(const std::filesystem::path& source, const std::filesystem::path& destination) {
#ifdef _WIN32
    // Omitting MOVEFILE_REPLACE_EXISTING preserves a destination created after
    // our initial check. Both paths are on the same volume.
    if (!MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
        throw std::filesystem::filesystem_error("cannot publish output", source, destination,
            std::error_code(static_cast<int>(GetLastError()), std::system_category()));
    }
#else
#if defined(__linux__) && defined(SYS_renameat2)
    if (::syscall(SYS_renameat2, AT_FDCWD, source.c_str(), AT_FDCWD,
                  destination.c_str(), RENAME_NOREPLACE) == 0) {
        return;
    }
    const int error = errno;
    if (error != ENOSYS && error != EINVAL && error != EOPNOTSUPP) {
        throw std::filesystem::filesystem_error("cannot publish output", source, destination,
            std::error_code(error, std::generic_category()));
    }
#endif
    // Portable POSIX fallback: linking a completed, closed file creates the
    // destination atomically and fails if any entry already occupies that path.
    std::filesystem::create_hard_link(source, destination);
#endif
}

} // namespace

TemporaryOutput::TemporaryOutput(const std::filesystem::path& destination)
    : destination_(std::filesystem::absolute(destination)) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(destination_, error);
    if (error && error != std::errc::no_such_file_or_directory) {
        throw std::filesystem::filesystem_error("cannot inspect output", destination_, error);
    }
    if (std::filesystem::exists(status)) {
        throw std::runtime_error("output already exists: " + destination_.string());
    }
    directory_ = make_directory(destination_.parent_path());
    try {
        payload_ = directory_ / "payload";
        stream_.open(payload_, std::ios::binary | std::ios::out | std::ios::trunc);
        if (!stream_) {
            throw std::runtime_error("cannot open temporary output");
        }
    } catch (...) {
        cleanup();
        throw;
    }
}

TemporaryOutput::~TemporaryOutput() { cleanup(); }

void TemporaryOutput::cleanup() noexcept {
    try {
        if (stream_.is_open()) {
            stream_.close();
        }
    } catch (...) {
        // Cleanup must preserve an exception already leaving the operation.
    }
    std::error_code ignored;
    if (!payload_.empty()) {
        std::filesystem::remove(payload_, ignored);
    }
    if (!directory_.empty()) {
        std::filesystem::remove(directory_, ignored);
    }
}

void TemporaryOutput::finish() {
    stream_.flush();
    if (!stream_) {
        throw std::runtime_error("failed to flush temporary output");
    }
    stream_.close();
    if (!stream_) {
        throw std::runtime_error("failed to close temporary output");
    }
    publish(payload_, destination_);
    cleanup();
}

} // namespace huffblock::detail
