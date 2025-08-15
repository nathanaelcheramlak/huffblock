#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace huffblock {

inline constexpr std::uint32_t default_block_size = 2U * 1024U * 1024U;
inline constexpr std::uint32_t max_block_size = 16U * 1024U * 1024U;
inline constexpr unsigned max_code_length = 32;

enum class Encoding : std::uint8_t { raw = 0, huffman = 1 };

// A block owns its body; it has no file I/O or shared mutable state.
// Huffman bodies contain a 256-byte length table followed by packed bits.
struct EncodedBlock {
    Encoding encoding = Encoding::raw;
    std::uint32_t original_size = 0;
    std::uint32_t checksum = 0;
    std::uint64_t bit_count = 0;
    std::vector<std::uint8_t> body;
};

std::uint32_t crc32(std::span<const std::uint8_t> bytes);
EncodedBlock compress_block(std::span<const std::uint8_t> input);
std::vector<std::uint8_t> decompress_block(const EncodedBlock& block);

// Validate header sizes before allocating or reading the body of a block.
void validate_block_sizes(Encoding encoding, std::uint32_t original_size,
                          std::uint64_t bit_count, std::uint64_t body_size);

} // namespace huffblock
