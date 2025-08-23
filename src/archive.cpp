#include "huffblock/archive.hpp"
#include "file_output.hpp"
#include "limits.hpp"
#include "ordered_pipeline.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <istream>
#include <optional>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>

namespace huffblock {
namespace {

constexpr std::array<std::uint8_t, 8> magic{'H', 'U', 'F', 'F', 'B', 'L', 'K', 0};
constexpr std::size_t file_header_size = 16;
constexpr std::size_t block_header_size = 24;
constexpr std::uint8_t end_marker = 255;

void put_le(std::span<std::uint8_t> bytes, std::size_t offset,
            std::uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i) {
        bytes[offset + i] = static_cast<std::uint8_t>(value & 0xFFU);
        value >>= 8U;
    }
}

std::uint64_t get_le(std::span<const std::uint8_t> bytes,
                     std::size_t offset, unsigned width) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i) {
        value |= std::uint64_t{bytes[offset + i]} << (8U * i);
    }
    return value;
}

void write_bytes(std::ostream& output, std::span<const std::uint8_t> bytes) {
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!output) {
        throw std::runtime_error("failed to write output");
    }
}

void read_exact(std::istream& input, std::span<std::uint8_t> bytes) {
    input.read(reinterpret_cast<char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (!input) {
        throw std::runtime_error(input.bad() ? "failed to read input" : "truncated archive");
    }
}

void flush_output(std::ostream& output) {
    output.flush();
    if (!output) {
        throw std::runtime_error("failed to flush output");
    }
}

void check_block_size(std::uint32_t block_size) {
    if (block_size == 0 || block_size > max_block_size) {
        throw std::invalid_argument("block size must be between 1 and 16777216 bytes");
    }
}

ExecutionOptions normalize(ExecutionOptions options) {
    if (options.threads > max_worker_threads || options.max_in_flight > max_in_flight_blocks) {
        throw std::invalid_argument("execution options exceed worker or in-flight limits");
    }
    if (options.threads == 0) {
        options.threads = std::min(32U, std::max(1U, std::thread::hardware_concurrency()));
    }
    if (options.max_in_flight == 0) {
        options.max_in_flight = 2U * options.threads;
    }
    return options;
}

ExecutionOptions bound_buffers(ExecutionOptions options, std::uint32_t block_size, bool decoding) {
    // Include simultaneous input and result ownership inside a codec call.
    const std::uint64_t per_block = decoding ? 5ULL * block_size + 256U : 2ULL * block_size;
    if (options.buffer_limit < per_block) {
        throw std::invalid_argument("--buffer-limit is too small for one block");
    }
    options.max_in_flight = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        options.max_in_flight, options.buffer_limit / per_block));
    if (options.threads == 1) {
        options.max_in_flight = 1;
    }
    return options;
}

void write_block(std::ostream& output, const EncodedBlock& block) {
    std::array<std::uint8_t, block_header_size> header{};
    header[0] = static_cast<std::uint8_t>(block.encoding);
    put_le(header, 4, block.original_size, 4);
    put_le(header, 8, block.body.size(), 4);
    put_le(header, 12, block.checksum, 4);
    put_le(header, 16, block.bit_count, 8);
    write_bytes(output, header);
    write_bytes(output, block.body);
}

std::optional<EncodedBlock> read_block(std::istream& input, std::uint32_t block_size,
                                      std::uint64_t& output_total, std::uint64_t output_limit) {
    std::array<std::uint8_t, block_header_size> header{};
    read_exact(input, header);
    if (get_le(header, 1, 3) != 0) {
        throw std::runtime_error("nonzero reserved block fields");
    }
    const auto original_size = static_cast<std::uint32_t>(get_le(header, 4, 4));
    const auto body_size = static_cast<std::uint32_t>(get_le(header, 8, 4));
    const auto checksum = static_cast<std::uint32_t>(get_le(header, 12, 4));
    const auto bit_count = get_le(header, 16, 8);
    if (header[0] == end_marker) {
        if (original_size != 0 || body_size != 0 || checksum != 0 || bit_count != 0) {
            throw std::runtime_error("invalid archive end marker");
        }
        if (input.peek() != std::char_traits<char>::eof()) {
            throw std::runtime_error("trailing data after archive end marker");
        }
        if (input.bad() || (input.fail() && !input.eof())) {
            throw std::runtime_error("failed to read archive end");
        }
        return std::nullopt;
    }
    if (original_size > block_size) {
        throw std::runtime_error("block exceeds archive block-size limit");
    }
    const auto encoding = static_cast<Encoding>(header[0]);
    validate_block_sizes(encoding, original_size, bit_count, body_size);
    output_total = detail::checked_output_total(output_total, original_size, output_limit);
    EncodedBlock block{encoding, original_size, checksum, bit_count,
                       std::vector<std::uint8_t>(body_size)};
    read_exact(input, block.body);
    return block;
}

std::ifstream open_input(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open input '" + path.string() + "'");
    }
    return input;
}

} // namespace

void compress_stream(std::istream& input, std::ostream& output, std::uint32_t block_size,
                     ExecutionOptions options) {
    check_block_size(block_size);
    options = bound_buffers(normalize(options), block_size, false);
    std::array<std::uint8_t, file_header_size> header{};
    std::copy(magic.begin(), magic.end(), header.begin());
    put_le(header, 8, 1, 2); // format version
    put_le(header, 12, block_size, 4);
    write_bytes(output, header);

    bool finished = false;
    detail::ordered_pipeline<std::vector<std::uint8_t>, EncodedBlock>(
        options.threads, options.max_in_flight,
        [&]() -> std::optional<std::vector<std::uint8_t>> {
            if (finished) {
                return std::nullopt;
            }
            std::vector<std::uint8_t> buffer(block_size);
            input.read(reinterpret_cast<char*>(buffer.data()),
                       static_cast<std::streamsize>(buffer.size()));
            if (input.bad() || (input.fail() && !input.eof())) {
                throw std::runtime_error("failed to read input");
            }
            finished = input.eof();
            const auto count = static_cast<std::size_t>(input.gcount());
            if (count == 0) {
                return std::nullopt;
            }
            buffer.resize(count);
            return buffer;
        },
        [](std::vector<std::uint8_t> buffer, std::stop_token) {
            return compress_block(buffer);
        },
        [&](EncodedBlock block) { write_block(output, block); });
    std::array<std::uint8_t, block_header_size> end{};
    end[0] = end_marker;
    write_bytes(output, end);
    flush_output(output);
}

void decompress_stream(std::istream& input, std::ostream& output, ExecutionOptions options) {
    options = normalize(options);
    std::array<std::uint8_t, file_header_size> header{};
    read_exact(input, header);
    if (!std::equal(magic.begin(), magic.end(), header.begin())) {
        throw std::runtime_error("invalid huffblock magic signature");
    }
    if (get_le(header, 8, 2) != 1) {
        throw std::runtime_error("unsupported archive version");
    }
    if (get_le(header, 10, 2) != 0) {
        throw std::runtime_error("unsupported archive flags");
    }
    const auto block_size = static_cast<std::uint32_t>(get_le(header, 12, 4));
    check_block_size(block_size);
    options = bound_buffers(options, block_size, true);
    std::uint64_t output_total = 0;

    detail::ordered_pipeline<EncodedBlock, std::vector<std::uint8_t>>(
        options.threads, options.max_in_flight,
        [&] { return read_block(input, block_size, output_total, options.max_output_size); },
        [](EncodedBlock block, std::stop_token) { return decompress_block(block); },
        [&](std::vector<std::uint8_t> buffer) { write_bytes(output, buffer); });
    flush_output(output);
}

void compress_file(const std::filesystem::path& input_path,
                   const std::filesystem::path& output_path, std::uint32_t block_size,
                   ExecutionOptions options) {
    check_block_size(block_size);
    options = normalize(options);
    auto input = open_input(input_path);
    detail::TemporaryOutput output(output_path);
    compress_stream(input, output.stream(), block_size, options);
    output.finish();
}

void decompress_file(const std::filesystem::path& input_path,
                     const std::filesystem::path& output_path, ExecutionOptions options) {
    options = normalize(options);
    auto input = open_input(input_path);
    detail::TemporaryOutput output(output_path);
    decompress_stream(input, output.stream(), options);
    output.finish();
}

} // namespace huffblock
