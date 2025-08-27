#include "huffblock/archive.hpp"
#include "huffblock/codec.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;
using huffblock::EncodedBlock;
using huffblock::Encoding;

void check(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

template <class Function>
void rejects(Function&& function) {
    bool threw = false;
    try {
        function();
    } catch (const std::exception&) {
        threw = true;
    }
    check(threw, "expected operation to reject invalid input");
}

Bytes bytes(std::string_view text) { return {text.begin(), text.end()}; }

std::string text(const Bytes& data) { return {data.begin(), data.end()}; }

std::string compress(const std::string& data, std::uint32_t size = 1024) {
    std::istringstream input(data);
    std::ostringstream output;
    huffblock::compress_stream(input, output, size, {.threads = 1});
    return output.str();
}

std::string decompress(const std::string& data) {
    std::istringstream input(data);
    std::ostringstream output;
    huffblock::decompress_stream(input, output, {.threads = 1});
    return output.str();
}

void put(Bytes& data, std::size_t offset, std::uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i) {
        data[offset + i] = static_cast<std::uint8_t>(value & 255U);
        value >>= 8U;
    }
}

// Independent ABC example: A=0, B=10, C=11, bits 01011, CRC-32 A3830348.
EncodedBlock abc_block() {
    EncodedBlock block{Encoding::huffman, 3, 0xA3830348U, 5, Bytes(257)};
    block.body['A'] = 1;
    block.body['B'] = 2;
    block.body['C'] = 2;
    block.body[256] = 0x58;
    return block;
}

Bytes empty_archive(std::uint32_t size = 1024) {
    Bytes data(40);
    const std::array<std::uint8_t, 8> signature{'H', 'U', 'F', 'F', 'B', 'L', 'K', 0};
    std::copy(signature.begin(), signature.end(), data.begin());
    put(data, 8, 1, 2);
    put(data, 12, size, 4);
    data[16] = 255;
    return data;
}

Bytes archive_of(const EncodedBlock& block) {
    auto data = empty_archive();
    data.resize(16);
    data.resize(40);
    data[16] = static_cast<std::uint8_t>(block.encoding);
    put(data, 20, block.original_size, 4);
    put(data, 24, block.body.size(), 4);
    put(data, 28, block.checksum, 4);
    put(data, 32, block.bit_count, 8);
    data.insert(data.end(), block.body.begin(), block.body.end());
    const auto end = data.size();
    data.resize(end + 24);
    data[end] = 255;
    return data;
}

void checksum_vectors() {
    check(huffblock::crc32({}) == 0, "empty CRC-32");
    check(huffblock::crc32(bytes("123456789")) == 0xCBF43926U, "standard CRC-32 vector");
    check(huffblock::crc32(bytes("ABC")) == 0xA3830348U, "ABC CRC-32 vector");
}

void known_huffman_codes() {
    check(huffblock::decompress_block(abc_block()) == bytes("ABC"), "known ABC bits");

    Bytes input(2048, 'A');
    input.insert(input.end(), 1024, 'B');
    input.insert(input.end(), 1024, 'C');
    const auto block = huffblock::compress_block(input);
    check(block.encoding == Encoding::huffman, "compressible block should use Huffman");
    check(block.bit_count == 6144 && block.body.size() == 1024, "known encoded size");
    Bytes expected(256);
    expected['A'] = 1;
    expected['B'] = 2;
    expected['C'] = 2;
    expected.insert(expected.end(), 256, 0x00);
    expected.insert(expected.end(), 256, 0xAA);
    expected.insert(expected.end(), 256, 0xFF);
    check(block.body == expected, "canonical encoder must match independently known bits");
    check(huffblock::decompress_block(block) == input, "known encoder round trip");
    check(huffblock::compress_block(input).body == block.body, "deterministic codebook");
}

void singleton_and_raw() {
    const Bytes input(4097, 'z');
    const auto block = huffblock::compress_block(input);
    check(block.encoding == Encoding::huffman, "single-symbol Huffman block");
    check(block.bit_count == input.size() && block.body['z'] == 1, "single-symbol length");
    check(std::all_of(block.body.begin() + 256, block.body.end(),
                      [](auto byte) { return byte == 0; }), "single-symbol zero code and padding");
    check(huffblock::decompress_block(block) == input, "single-symbol round trip");
    const auto tiny = huffblock::compress_block(bytes("x"));
    check(tiny.encoding == Encoding::raw && tiny.body == bytes("x"), "tiny raw block");
    Bytes uniform;
    for (unsigned i = 0; i < 4096; ++i) {
        uniform.push_back(static_cast<std::uint8_t>(i % 256));
    }
    const auto raw = huffblock::compress_block(uniform);
    check(raw.encoding == Encoding::raw && raw.body == uniform, "uniform bytes use raw storage");
    check(huffblock::decompress_block(raw) == uniform, "raw round trip");
    // 256 length bytes + 37 packed bytes = 293 input bytes: ties use raw.
    check(huffblock::compress_block(Bytes(293, 0)).encoding == Encoding::raw,
          "equal-size encoding uses raw");
    check(huffblock::compress_block(Bytes(294, 0)).encoding == Encoding::huffman,
          "first smaller single-symbol encoding");
}

void maximum_length_code() {
    EncodedBlock block{Encoding::huffman, 1, 0, 32, Bytes(260, 0)};
    for (unsigned symbol = 0; symbol < 31; ++symbol) {
        block.body[symbol] = static_cast<std::uint8_t>(symbol + 1);
    }
    block.body[31] = 32;
    block.body[32] = 32;
    std::fill(block.body.begin() + 256, block.body.end(), 0xFF);
    block.checksum = huffblock::crc32(Bytes{32});
    check(huffblock::decompress_block(block) == Bytes{32}, "32-bit all-ones code");
}

void oversized_code_fallback() {
    // These 34 Fibonacci frequencies fit in a legal block but produce a
    // depth-33 Huffman tree. The encoder must store raw, not truncate a code.
    std::array<std::uint32_t, 34> frequencies{1, 1};
    for (std::size_t i = 2; i < frequencies.size(); ++i) {
        frequencies[i] = frequencies[i - 1] + frequencies[i - 2];
    }
    Bytes input;
    for (std::size_t symbol = 0; symbol < frequencies.size(); ++symbol) {
        input.insert(input.end(), frequencies[symbol], static_cast<std::uint8_t>(symbol));
    }
    check(input.size() <= huffblock::max_block_size, "pathological input fits block limit");
    const auto block = huffblock::compress_block(input);
    check(block.encoding == Encoding::raw, "overlong Huffman codes require raw fallback");
    check(huffblock::decompress_block(block) == input, "overlong-code fallback round trip");
}

void longest_encoder_codes() {
    // 33 Fibonacci frequencies create legal depth-32 codes. Put seven one-bit
    // symbols before the rare symbols to exercise the writer's 39-bit maximum.
    std::array<std::uint32_t, 33> frequencies{1, 1};
    for (std::size_t i = 2; i < frequencies.size(); ++i) {
        frequencies[i] = frequencies[i - 1] + frequencies[i - 2];
    }
    Bytes input;
    for (std::size_t symbol = 0; symbol < frequencies.size(); ++symbol) {
        input.insert(input.end(), frequencies[symbol], static_cast<std::uint8_t>(symbol));
    }
    std::rotate(input.begin(), input.end() - 7, input.end());
    const auto block = huffblock::compress_block(input);
    check(block.encoding == Encoding::huffman && block.body[0] == 32 && block.body[1] == 32 &&
          block.body[32] == 1, "encoder must retain legal 32-bit codes");
    const std::array<std::uint8_t, 5> prefix{0x01, 0xFF, 0xFF, 0xFF, 0xFD};
    check(std::equal(prefix.begin(), prefix.end(), block.body.begin() + 256),
          "known cross-byte 32-bit code packing");
    check(huffblock::decompress_block(block) == input, "32-bit encoder round trip");
}

void randomized_blocks() {
    std::mt19937 random(0xC0FFEEU);
    for (unsigned alphabet : {1U, 2U, 3U, 17U, 256U}) {
        for (unsigned size : {1U, 7U, 255U, 256U, 1023U, 8193U}) {
            Bytes input(size);
            for (auto& byte : input) {
                byte = static_cast<std::uint8_t>(random() % alphabet);
            }
            const auto block = huffblock::compress_block(input);
            check(huffblock::decompress_block(block) == input, "seeded block round trip");
            check(block.body.size() <= input.size(), "stored body must not expand");
        }
    }
    rejects([] { huffblock::compress_block({}); });
    rejects([] { huffblock::compress_block(Bytes(huffblock::max_block_size + 1U)); });
}

void malformed_blocks() {
    const auto original = abc_block();
    const auto mutate = [&](const auto& change) {
        auto block = original;
        change(block);
        rejects([&] { huffblock::decompress_block(block); });
    };
    mutate([](auto& b) { b.original_size = 0; });
    mutate([](auto& b) { b.original_size = huffblock::max_block_size + 1; });
    mutate([](auto& b) { b.encoding = static_cast<Encoding>(2); });
    mutate([](auto& b) { b.bit_count = std::numeric_limits<std::uint64_t>::max(); });
    mutate([](auto& b) { b.bit_count = 0; });
    mutate([](auto& b) { b.body.pop_back(); });
    mutate([](auto& b) { b.body.push_back(0); });
    mutate([](auto& b) { b.checksum ^= 1; });
    mutate([](auto& b) { b.body['A'] = 33; });
    mutate([](auto& b) { b.body['A'] = b.body['B'] = b.body['C'] = 1; });
    mutate([](auto& b) { b.body['A'] = b.body['B'] = b.body['C'] = 2; });
    mutate([](auto& b) { std::fill_n(b.body.begin(), 256, 0); });
    mutate([](auto& b) { std::fill_n(b.body.begin(), 256, 0); b.body['A'] = 2; });
    mutate([](auto& b) { b.body.back() |= 1; });
    mutate([](auto& b) { b.bit_count = 6; }); // extra decoded A
    mutate([](auto& b) { b.original_size = 4; }); // too few decoded bytes
    mutate([](auto& b) { b.original_size = 1; b.bit_count = 1; b.body.back() = 0x80; });
    mutate([](auto& b) {
        std::fill_n(b.body.begin(), 256, 0);
        b.body['A'] = 1;
        b.bit_count = 3;
        b.body.back() = 0x80; // undefined branch of singleton tree
    });
    auto raw = huffblock::compress_block(bytes("raw"));
    raw.body[0] ^= 1;
    rejects([&] { huffblock::decompress_block(raw); });
    raw.body.push_back(0);
    rejects([&] { huffblock::decompress_block(raw); });
    rejects([] { huffblock::validate_block_sizes(Encoding::raw, 1, 7, 1); });
}

void known_archives() {
    check(compress("") == text(empty_archive()), "exact empty archive bytes");
    check(decompress(text(empty_archive())).empty(), "independent empty archive");
    check(decompress(text(archive_of(abc_block()))) == "ABC", "independent Huffman archive");
    const EncodedBlock raw{Encoding::raw, 9, 0xCBF43926U, 72, bytes("123456789")};
    const auto expected = text(archive_of(raw));
    check(compress("123456789") == expected, "exact raw archive framing and endianness");
    check(decompress(expected) == "123456789", "independent raw archive");
}

void archive_round_trips() {
    std::mt19937 random(12345);
    for (unsigned size : {0U, 1U, 7U, 255U, 1023U, 1024U, 1025U, 2048U, 8193U}) {
        std::string input(size, '\0');
        for (auto& byte : input) {
            byte = static_cast<char>(random() & 255U);
        }
        check(decompress(compress(input)) == input, "raw multiblock boundary round trip");
        check(decompress(compress(input, 1)) == input, "one-byte blocks");
    }
    std::string mixed(4096, 'A');
    for (unsigned i = 0; i < 1024; ++i) {
        mixed.push_back(static_cast<char>(i % 256));
    }
    mixed.append(1031, 'B');
    check(decompress(compress(mixed)) == mixed, "mixed raw and Huffman blocks");
    const std::string large(2U * huffblock::default_block_size + 37U, 'L');
    check(decompress(compress(large, huffblock::default_block_size)) == large,
          "default-sized blocks plus partial final block");
    const std::string maximum(huffblock::max_block_size, 'M');
    check(decompress(compress(maximum, huffblock::max_block_size)) == maximum,
          "maximum supported block size");
    rejects([] { compress("", 0); });
    rejects([] { compress("", huffblock::max_block_size + 1); });
}

void malformed_archives() {
    const auto valid = archive_of(abc_block());
    for (std::size_t size = 0; size < valid.size(); ++size) {
        rejects([&] { decompress(text(Bytes(valid.begin(), valid.begin() +
                                                       static_cast<std::ptrdiff_t>(size)))); });
    }
    const auto mutate = [&](const auto& change) {
        auto data = valid;
        change(data);
        rejects([&] { decompress(text(data)); });
    };
    mutate([](auto& d) { d[0] ^= 1; });
    mutate([](auto& d) { d[7] = 1; });
    mutate([](auto& d) { put(d, 8, 2, 2); });
    mutate([](auto& d) { put(d, 10, 1, 2); });
    mutate([](auto& d) { put(d, 12, 0, 4); });
    mutate([](auto& d) { put(d, 12, huffblock::max_block_size + 1, 4); });
    mutate([](auto& d) { put(d, 12, 2, 4); });
    mutate([](auto& d) { d[16] = 2; });
    mutate([](auto& d) { d[17] = 1; });
    mutate([](auto& d) { put(d, 20, 0xFFFFFFFFU, 4); });
    mutate([](auto& d) { put(d, 24, 0xFFFFFFFFU, 4); });
    mutate([](auto& d) { put(d, 32, std::numeric_limits<std::uint64_t>::max(), 8); });
    mutate([](auto& d) { d[28] ^= 1; });
    mutate([](auto& d) { d[d.size() - 23] = 1; });
    mutate([](auto& d) { d[d.size() - 20] = 1; });
    mutate([](auto& d) { d[d.size() - 16] = 1; });
    mutate([](auto& d) { d[d.size() - 12] = 1; });
    mutate([](auto& d) { d[d.size() - 8] = 1; });
    mutate([](auto& d) { d.push_back(0); });
    mutate([&](auto& d) { d.insert(d.end(), valid.begin(), valid.end()); });
    // EOF directly after a complete data block still lacks the required marker.
    rejects([&] { decompress(text(Bytes(valid.begin(), valid.end() - 24))); });
}

class FailedWriteBuffer : public std::streambuf {
protected:
    std::streamsize xsputn(const char*, std::streamsize) override { return 0; }
};

class FailedFlushBuffer : public std::stringbuf {
protected:
    int sync() override { return -1; }
};

void stream_failures() {
    FailedWriteBuffer failed;
    std::ostream output(&failed);
    std::istringstream input("hello");
    rejects([&] { huffblock::compress_stream(input, output); });
    std::istringstream archive(compress("hello"));
    std::ostream output2(&failed);
    rejects([&] { huffblock::decompress_stream(archive, output2); });
    FailedFlushBuffer flush;
    std::ostream output3(&flush);
    std::istringstream empty;
    rejects([&] { huffblock::compress_stream(empty, output3); });
    std::istringstream empty_archive_input(compress(""));
    std::ostream output4(&flush);
    rejects([&] { huffblock::decompress_stream(empty_archive_input, output4); });
    std::istringstream bad_input;
    bad_input.setstate(std::ios::badbit);
    std::ostringstream sink;
    rejects([&] { huffblock::compress_stream(bad_input, sink); });
    rejects([&] { huffblock::decompress_stream(bad_input, sink); });
}

class TestDirectory {
public:
    TestDirectory() {
        const auto parent = std::filesystem::current_path();
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        path = parent / ("huffblock-test-" + std::to_string(nonce));
        check(path.parent_path() == parent && std::filesystem::create_directory(path),
              "create isolated test directory beneath build directory");
    }
    ~TestDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    std::filesystem::path path;
};

void write_file(const std::filesystem::path& path, std::string_view data) {
    std::ofstream output(path, std::ios::binary);
    output.write(data.data(), static_cast<std::streamsize>(data.size()));
    output.close();
    check(static_cast<bool>(output), "write test file");
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    check(static_cast<bool>(input), "open test file");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void file_safety() {
    TestDirectory directory;
    const auto input = directory.path / "original.bin";
    const auto archive = directory.path / "archive.hfb";
    const auto restored = directory.path / "restored.bin";
    const std::string original(8193, 'A');
    write_file(input, original);
    huffblock::compress_file(input, archive, 1024);
    huffblock::decompress_file(archive, restored);
    check(read_file(input) == original && read_file(restored) == original, "file round trip");
    const auto archive_before = read_file(archive);
    rejects([&] { huffblock::compress_file(input, archive); });
    check(read_file(archive) == archive_before, "existing archive preserved");
    rejects([&] { huffblock::decompress_file(archive, restored); });
    check(read_file(restored) == original, "existing restored file preserved");
    rejects([&] { huffblock::compress_file(input, input); });
    rejects([&] { huffblock::decompress_file(archive, archive); });
    check(read_file(input) == original, "same-path input preserved");

    const auto failed = directory.path / "failed.bin";
    const auto broken = directory.path / "broken.hfb";
    write_file(broken, archive_before.substr(0, archive_before.size() - 24));
    rejects([&] { huffblock::decompress_file(broken, failed); });
    check(!std::filesystem::exists(failed), "partial output removed after late failure");
    rejects([&] { huffblock::compress_file(directory.path / "missing", failed); });
    check(!std::filesystem::exists(failed), "missing input creates no output");
    rejects([&] { huffblock::compress_file(input, failed, 0); });
    check(!std::filesystem::exists(failed), "invalid block size creates no output");

    const auto alias = directory.path / "alias.bin";
    std::filesystem::create_hard_link(input, alias);
    rejects([&] { huffblock::compress_file(input, alias); });
    check(read_file(input) == original, "hard-linked input preserved");
    rejects([&] { huffblock::compress_file(input, directory.path / "missing" / "out"); });
}

} // namespace

int main() {
    const std::vector<std::pair<std::string_view, std::function<void()>>> tests{
        {"CRC-32 known vectors", checksum_vectors},
        {"independently known canonical codes", known_huffman_codes},
        {"single symbols and raw fallback", singleton_and_raw},
        {"maximum 32-bit canonical code", maximum_length_code},
        {"raw fallback for codes beyond 32 bits", oversized_code_fallback},
        {"unaligned 32-bit encoder codes", longest_encoder_codes},
        {"seeded block round trips", randomized_blocks},
        {"malformed blocks", malformed_blocks},
        {"independently specified archive bytes", known_archives},
        {"archive boundaries and mixed encodings", archive_round_trips},
        {"malformed and truncated archives", malformed_archives},
        {"stream read, write, and flush failures", stream_failures},
        {"file preservation and failure cleanup", file_safety},
    };
    unsigned failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << tests.size() - failures << '/' << tests.size() << " test groups passed\n";
    return failures == 0 ? 0 : 1;
}
