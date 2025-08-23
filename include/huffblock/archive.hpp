#pragma once

#include "huffblock/codec.hpp"

#include <filesystem>
#include <iosfwd>
#include <limits>

namespace huffblock {

inline constexpr std::uint32_t max_worker_threads = 256;
inline constexpr std::uint32_t max_in_flight_blocks = 1024;

struct ExecutionOptions {
    // Zero selects min(hardware concurrency, 32), with a fallback of one.
    // One executes entirely on the calling thread.
    std::uint32_t threads = 0;
    // Zero selects twice the worker count. Includes the block being read and
    // results waiting for or currently undergoing ordered output.
    std::uint32_t max_in_flight = 0;
    // Bounds block payload buffers, not total RSS (stacks/scratch are extra).
    std::uint64_t buffer_limit = 256ULL * 1024ULL * 1024ULL;
    // Decompression only. Zero permits only empty output.
    std::uint64_t max_output_size = std::numeric_limits<std::uint64_t>::max();
};

// Neither function closes its caller's streams. A stream may contain partial
// output on error; use the file wrappers for publication only on success.
void compress_stream(std::istream& input, std::ostream& output,
                     std::uint32_t block_size = default_block_size,
                     ExecutionOptions options = {});
void decompress_stream(std::istream& input, std::ostream& output,
                       ExecutionOptions options = {});

// File operations use sibling temporary files and publish completed output
// without replacing an existing path, including one created during processing.
void compress_file(const std::filesystem::path& input,
                   const std::filesystem::path& output,
                   std::uint32_t block_size = default_block_size,
                   ExecutionOptions options = {});
void decompress_file(const std::filesystem::path& input,
                     const std::filesystem::path& output,
                     ExecutionOptions options = {});

} // namespace huffblock
