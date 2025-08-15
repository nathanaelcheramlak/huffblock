#include "huffblock/codec.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <queue>
#include <stdexcept>
#include <utility>

namespace huffblock {
namespace {

using Lengths = std::array<std::uint8_t, 256>;

struct Code {
    std::uint32_t bits = 0;
    unsigned length = 0;
};

std::array<Code, 256> canonical_codes(const Lengths& lengths) {
    std::array<std::uint64_t, max_code_length + 1> counts{};
    unsigned symbol_count = 0;
    for (auto length : lengths) {
        if (length > max_code_length) {
            throw std::runtime_error("code length exceeds 32 bits");
        }
        if (length != 0) {
            ++counts[length];
            ++symbol_count;
        }
    }
    if (symbol_count == 0) {
        throw std::runtime_error("empty Huffman codebook");
    }

    // Each unused prefix has two children at the next depth. Negative space
    // would mean overlapping codes; leftover space means an incomplete tree.
    std::uint64_t available = 1;
    for (unsigned length = 1; length <= max_code_length; ++length) {
        available *= 2;
        if (counts[length] > available) {
            throw std::runtime_error("oversubscribed Huffman codebook");
        }
        available -= counts[length];
    }
    if (symbol_count == 1) {
        if (counts[1] != 1) {
            throw std::runtime_error("single-symbol code must have length 1");
        }
    } else if (available != 0) {
        throw std::runtime_error("incomplete Huffman codebook");
    }

    std::array<std::uint64_t, max_code_length + 1> next{};
    std::uint64_t first = 0;
    for (unsigned length = 1; length <= max_code_length; ++length) {
        first = (first + counts[length - 1]) << 1U;
        next[length] = first;
    }
    std::array<Code, 256> result{};
    for (std::size_t symbol = 0; symbol < lengths.size(); ++symbol) {
        const auto length = lengths[symbol];
        if (length != 0) {
            result[symbol] = {static_cast<std::uint32_t>(next[length]++), length};
        }
    }
    return result;
}

struct TreeNode {
    std::uint64_t frequency;
    unsigned minimum_symbol;
    int left = -1;
    int right = -1;
    int symbol = -1;
};

Lengths make_lengths(std::span<const std::uint8_t> input) {
    std::array<std::uint64_t, 256> frequencies{};
    for (auto byte : input) {
        ++frequencies[byte];
    }
    std::vector<TreeNode> nodes;
    nodes.reserve(511);
    const auto later = [&nodes](int a, int b) {
        const auto& x = nodes[static_cast<std::size_t>(a)];
        const auto& y = nodes[static_cast<std::size_t>(b)];
        if (x.frequency != y.frequency) {
            return x.frequency > y.frequency;
        }
        return x.minimum_symbol > y.minimum_symbol;
    };
    std::priority_queue<int, std::vector<int>, decltype(later)> queue(later);
    for (unsigned symbol = 0; symbol < frequencies.size(); ++symbol) {
        if (frequencies[symbol] != 0) {
            nodes.push_back({frequencies[symbol], symbol, -1, -1,
                             static_cast<int>(symbol)});
            queue.push(static_cast<int>(nodes.size() - 1));
        }
    }
    while (queue.size() > 1) {
        const int left = queue.top();
        queue.pop();
        const int right = queue.top();
        queue.pop();
        const auto& a = nodes[static_cast<std::size_t>(left)];
        const auto& b = nodes[static_cast<std::size_t>(right)];
        nodes.push_back({a.frequency + b.frequency,
                         std::min(a.minimum_symbol, b.minimum_symbol), left, right, -1});
        queue.push(static_cast<int>(nodes.size() - 1));
    }

    Lengths lengths{};
    std::vector<std::pair<int, unsigned>> stack{{queue.top(), 0}};
    while (!stack.empty()) {
        const auto [index, depth] = stack.back();
        stack.pop_back();
        const auto& node = nodes[static_cast<std::size_t>(index)];
        if (node.symbol >= 0) {
            lengths[static_cast<std::size_t>(node.symbol)] =
                static_cast<std::uint8_t>(std::max(1U, depth));
        } else {
            stack.emplace_back(node.right, depth + 1);
            stack.emplace_back(node.left, depth + 1);
        }
    }
    return lengths;
}

class BitWriter {
public:
    explicit BitWriter(std::vector<std::uint8_t>& bytes) : bytes_(bytes) {}

    void write(Code code) {
#ifdef HUFFBLOCK_REFERENCE_CODEC
        for (unsigned remaining = code.length; remaining > 0; --remaining) {
            current_ = static_cast<std::uint8_t>(
                (static_cast<unsigned>(current_) << 1U) |
                ((code.bits >> (remaining - 1U)) & 1U));
            ++used_;
            ++bit_count_;
            if (used_ == 8) {
                bytes_.push_back(current_);
                current_ = 0;
                used_ = 0;
            }
        }
#else
        // At entry used_ < 8 and a code is at most 32 bits, so the reservoir
        // needs at most 39 bits. Never shift a 32-bit integer by 32.
        pending_ = (pending_ << code.length) | code.bits;
        used_ += code.length;
        bit_count_ += code.length;
        while (used_ >= 8) {
            used_ -= 8;
            bytes_.push_back(static_cast<std::uint8_t>((pending_ >> used_) & 255U));
        }
        pending_ &= (std::uint64_t{1} << used_) - 1U;
#endif
    }

    std::uint64_t finish() {
        if (used_ != 0) {
#ifdef HUFFBLOCK_REFERENCE_CODEC
            bytes_.push_back(static_cast<std::uint8_t>(static_cast<unsigned>(current_) << (8U - used_)));
#else
            bytes_.push_back(static_cast<std::uint8_t>(pending_ << (8U - used_)));
#endif
        }
        return bit_count_;
    }

private:
    std::vector<std::uint8_t>& bytes_;
#ifdef HUFFBLOCK_REFERENCE_CODEC
    std::uint8_t current_ = 0;
#else
    std::uint64_t pending_ = 0;
#endif
    unsigned used_ = 0;
    std::uint64_t bit_count_ = 0;
};

struct DecodeNode {
    std::array<int, 2> child{-1, -1};
    int symbol = -1;
};

#ifndef HUFFBLOCK_REFERENCE_CODEC
class BitReader {
public:
    BitReader(std::span<const std::uint8_t> bytes, std::uint64_t limit)
        : bytes_(bytes), limit_(limit) {}

    std::uint64_t remaining() const { return limit_ - consumed_; }

    unsigned peek(unsigned width) {
        if (width > remaining()) {
            throw std::runtime_error("incomplete Huffman symbol");
        }
        while (available_ < width) {
            if (next_ == bytes_.size()) {
                throw std::runtime_error("truncated Huffman bits");
            }
            pending_ = (pending_ << 8U) | bytes_[next_++];
            available_ += 8;
        }
        return static_cast<unsigned>((pending_ >> (available_ - width)) &
                                     ((std::uint64_t{1} << width) - 1U));
    }

    void consume(unsigned width) {
        available_ -= width; // only called after a successful peek
        consumed_ += width;
        pending_ &= (std::uint64_t{1} << available_) - 1U;
    }

private:
    std::span<const std::uint8_t> bytes_;
    std::uint64_t limit_;
    std::uint64_t consumed_ = 0;
    std::uint64_t pending_ = 0;
    std::size_t next_ = 0;
    unsigned available_ = 0;
};

constexpr unsigned lookup_bits = 10;
struct DecodeEntry {
    int symbol = -1;
    unsigned length = 0;
    int node = -1;
};
#endif

} // namespace

std::uint32_t crc32(std::span<const std::uint8_t> bytes) {
    static constexpr auto table = [] {
        std::array<std::uint32_t, 256> result{};
        for (std::uint32_t i = 0; i < result.size(); ++i) {
            auto value = i;
            for (unsigned bit = 0; bit < 8; ++bit) {
                value = (value >> 1U) ^ ((value & 1U) != 0 ? 0xEDB88320U : 0U);
            }
            result[i] = value;
        }
        return result;
    }();
    std::uint32_t value = 0xFFFFFFFFU;
    for (auto byte : bytes) {
        value = table[(value ^ byte) & 0xFFU] ^ (value >> 8U);
    }
    return value ^ 0xFFFFFFFFU;
}

void validate_block_sizes(Encoding encoding, std::uint32_t original_size,
                          std::uint64_t bit_count, std::uint64_t body_size) {
    if (original_size == 0 || original_size > max_block_size) {
        throw std::runtime_error("invalid uncompressed block size");
    }
    if (encoding == Encoding::raw) {
        if (body_size != original_size || bit_count != std::uint64_t{original_size} * 8U) {
            throw std::runtime_error("invalid raw block sizes");
        }
    } else if (encoding == Encoding::huffman) {
        if (bit_count < original_size ||
            bit_count > std::uint64_t{original_size} * max_code_length ||
            body_size != 256U + (bit_count + 7U) / 8U) {
            throw std::runtime_error("invalid Huffman block sizes");
        }
    } else {
        throw std::runtime_error("unknown block encoding");
    }
}

EncodedBlock compress_block(std::span<const std::uint8_t> input) {
    if (input.empty() || input.size() > max_block_size) {
        throw std::invalid_argument("block size must be between 1 and 16777216 bytes");
    }
    EncodedBlock result;
    result.original_size = static_cast<std::uint32_t>(input.size());
    result.checksum = crc32(input);

    const auto lengths = make_lengths(input);
    std::uint64_t bit_count = 0;
    bool supported = true;
    for (auto length : lengths) {
        supported = supported && length <= max_code_length;
    }
    for (auto byte : input) {
        bit_count += lengths[byte];
    }
    if (!supported || 256U + (bit_count + 7U) / 8U >= input.size()) {
        result.bit_count = std::uint64_t{input.size()} * 8U;
        result.body.assign(input.begin(), input.end());
        return result;
    }

    result.encoding = Encoding::huffman;
    result.body.reserve(static_cast<std::size_t>(256U + (bit_count + 7U) / 8U));
    result.body.insert(result.body.end(), lengths.begin(), lengths.end());
    const auto codes = canonical_codes(lengths);
    BitWriter writer(result.body);
    for (auto byte : input) {
        writer.write(codes[byte]);
    }
    result.bit_count = writer.finish();
    return result;
}

std::vector<std::uint8_t> decompress_block(const EncodedBlock& block) {
    validate_block_sizes(block.encoding, block.original_size, block.bit_count, block.body.size());
    std::vector<std::uint8_t> output;
    if (block.encoding == Encoding::raw) {
        output = block.body;
    } else {
        Lengths lengths{};
        std::copy_n(block.body.begin(), lengths.size(), lengths.begin());
        const auto codes = canonical_codes(lengths);
        std::vector<DecodeNode> tree(1);
#ifndef HUFFBLOCK_REFERENCE_CODEC
        tree.reserve(511);
#endif
        for (unsigned symbol = 0; symbol < codes.size(); ++symbol) {
            const auto code = codes[symbol];
            int node = 0;
            for (unsigned remaining = code.length; remaining > 0; --remaining) {
                const unsigned bit = (code.bits >> (remaining - 1U)) & 1U;
                int child = tree[static_cast<std::size_t>(node)].child[bit];
                if (child < 0) {
                    child = static_cast<int>(tree.size());
                    tree[static_cast<std::size_t>(node)].child[bit] = child;
                    tree.emplace_back();
                }
                node = child;
            }
            if (code.length != 0) {
                tree[static_cast<std::size_t>(node)].symbol = static_cast<int>(symbol);
            }
        }
        const unsigned padding = static_cast<unsigned>((8U - block.bit_count % 8U) % 8U);
        if ((block.body.back() & ((1U << padding) - 1U)) != 0) {
            throw std::runtime_error("nonzero Huffman padding bits");
        }
#ifdef HUFFBLOCK_REFERENCE_CODEC
        output.reserve(block.original_size);
        int node = 0;
        for (std::uint64_t position = 0; position < block.bit_count; ++position) {
            const auto byte = block.body[256U + static_cast<std::size_t>(position / 8U)];
            const unsigned shift = 7U - static_cast<unsigned>(position % 8U);
            node = tree[static_cast<std::size_t>(node)].child[(byte >> shift) & 1U];
            if (node < 0) {
                throw std::runtime_error("invalid Huffman bit sequence");
            }
            const int symbol = tree[static_cast<std::size_t>(node)].symbol;
            if (symbol >= 0) {
                if (output.size() == block.original_size) {
                    throw std::runtime_error("Huffman block produces too many bytes");
                }
                output.push_back(static_cast<std::uint8_t>(symbol));
                node = 0;
            }
        }
        if (node != 0) {
            throw std::runtime_error("incomplete Huffman symbol");
        }
#else
        std::array<DecodeEntry, 1U << lookup_bits> lookup{};
        for (unsigned symbol = 0; symbol < codes.size(); ++symbol) {
            const auto code = codes[symbol];
            if (code.length == 0) {
                continue;
            }
            if (code.length <= lookup_bits) {
                const unsigned extra = lookup_bits - code.length;
                const auto first = code.bits << extra;
                for (unsigned i = 0; i < (1U << extra); ++i) {
                    lookup[first + i] = {static_cast<int>(symbol), code.length, -1};
                }
            } else {
                const auto prefix = code.bits >> (code.length - lookup_bits);
                int node = 0;
                for (unsigned i = lookup_bits; i > 0; --i) {
                    node = tree[static_cast<std::size_t>(node)].child[(prefix >> (i - 1U)) & 1U];
                }
                lookup[prefix].node = node;
            }
        }
        output.reserve(block.original_size);
        BitReader reader(std::span(block.body).subspan(256), block.bit_count);
        while (reader.remaining() != 0) {
            if (output.size() == block.original_size) {
                throw std::runtime_error("Huffman block produces too many bytes");
            }
            int node = 0;
            if (reader.remaining() >= lookup_bits) {
                const auto entry = lookup[reader.peek(lookup_bits)];
                if (entry.length != 0) {
                    reader.consume(entry.length);
                    output.push_back(static_cast<std::uint8_t>(entry.symbol));
                    continue;
                }
                if (entry.node < 0) {
                    throw std::runtime_error("invalid Huffman bit sequence");
                }
                reader.consume(lookup_bits);
                node = entry.node;
            }
            // Long codes and the final <10 meaningful bits use the tree.
            while (tree[static_cast<std::size_t>(node)].symbol < 0) {
                const auto bit = reader.peek(1);
                reader.consume(1);
                node = tree[static_cast<std::size_t>(node)].child[bit];
                if (node < 0) {
                    throw std::runtime_error("invalid Huffman bit sequence");
                }
            }
            output.push_back(static_cast<std::uint8_t>(tree[static_cast<std::size_t>(node)].symbol));
        }
#endif
        if (output.size() != block.original_size) {
            throw std::runtime_error("Huffman block has incomplete or missing symbols");
        }
    }
    if (crc32(output) != block.checksum) {
        throw std::runtime_error("block checksum mismatch");
    }
    return output;
}

} // namespace huffblock
