#include "huffblock/archive.hpp"
#include "limits.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <class Function>
void rejects(Function&& function) {
    try {
        function();
    } catch (const std::runtime_error&) {
        return;
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error("invalid input was accepted");
}

std::string compress(const std::string& data, std::uint32_t block_size = 512) {
    std::istringstream input(data);
    std::ostringstream output;
    huffblock::compress_stream(input, output, block_size, {.threads = 1});
    return output.str();
}

std::string decode(const std::string& data, huffblock::ExecutionOptions options) {
    std::istringstream input(data);
    std::ostringstream output;
    huffblock::decompress_stream(input, output, options);
    return output.str();
}

void resource_limits() {
    const auto archive = compress(std::string(2049, 'A'));
    for (std::uint32_t workers : {1U, 4U}) {
        huffblock::ExecutionOptions options{workers, 8};
        options.max_output_size = 2049;
        check(decode(archive, options) == std::string(2049, 'A'), "exact output limit");
        options.max_output_size = 2048;
        rejects([&] { decode(archive, options); });
        options.max_output_size = 0;
        rejects([&] { decode(archive, options); });
        check(decode(compress(""), options).empty(), "zero budget accepts empty output");
        options.max_output_size = 2049;
        options.buffer_limit = 5U * 512U + 256U;
        check(decode(archive, options) == std::string(2049, 'A'), "buffer budget clamps admission to one");
        --options.buffer_limit;
        rejects([&] { decode(archive, options); });
        std::istringstream input("hello");
        std::ostringstream output;
        options.buffer_limit = 1023;
        rejects([&] { huffblock::compress_stream(input, output, 512, options); });
        check(output.str().empty(), "invalid compression budget rejected before output header");
    }
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    check(huffblock::detail::checked_output_total(maximum - 1, 1, maximum) == maximum,
          "exact uint64 limit");
    rejects([&] { huffblock::detail::checked_output_total(maximum, 1, maximum); });
    rejects([&] { huffblock::detail::checked_output_total(5, 0, 4); });
    rejects([&] { huffblock::detail::checked_output_total(1, maximum, maximum); });
}

void mutation_campaign() {
    std::mt19937 random(0x48554646U);
    std::string uniform(4097, '\0');
    std::string skewed(4097, '\0');
    for (std::size_t i = 0; i < uniform.size(); ++i) {
        uniform[i] = static_cast<char>(random() & 255U);
        skewed[i] = static_cast<char>(random() % 5U);
    }
    const std::vector<std::string> sources{"", "x", std::string(4097, 'A'), uniform, skewed};
    std::vector<std::string> corpus;
    for (const auto& source : sources) {
        corpus.push_back(compress(source));
    }
    const huffblock::ExecutionOptions options{.threads = 1, .max_in_flight = 1,
        .buffer_limit = 1024U * 1024U, .max_output_size = 65536};
    std::uint64_t accepted = 0;
    std::uint64_t rejected = 0;
    const auto probe = [&](const std::string& candidate) {
        std::string restored;
        try {
            restored = decode(candidate, options);
        } catch (const std::runtime_error&) {
            ++rejected;
            return;
        } catch (const std::invalid_argument&) {
            ++rejected;
            return;
        }
        check(restored.size() <= options.max_output_size, "decoder exceeded output budget");
        ++accepted;
        // bad_alloc, length_error, sanitizer failures, and other unexpected
        // exceptions are deliberately NOT swallowed by the campaign.
    };
    for (std::size_t i = 0; i < corpus.size(); ++i) {
        check(decode(corpus[i], options) == sources[i], "valid mutation seed");
        for (std::size_t length = 0; length < corpus[i].size(); ++length) {
            rejects([&] { decode(corpus[i].substr(0, length), options); });
            ++rejected;
        }
    }
    for (unsigned iteration = 0; iteration < 5000; ++iteration) {
        auto candidate = corpus[random() % corpus.size()];
        switch (iteration % 5U) {
        case 0:
            candidate[random() % candidate.size()] ^= static_cast<char>(1U << (random() % 8U));
            break;
        case 1:
            for (unsigned edit = 0; edit < 4; ++edit) {
                candidate[random() % candidate.size()] = static_cast<char>(random() & 255U);
            }
            break;
        case 2:
            candidate.insert(random() % candidate.size(), 1, static_cast<char>(random() & 255U));
            break;
        case 3:
            candidate.erase(random() % candidate.size(), 1);
            break;
        default:
            candidate.assign(random() % 2048U, '\0');
            for (auto& byte : candidate) {
                byte = static_cast<char>(random() & 255U);
            }
        }
        probe(candidate);
    }
    // Exercise the same parser errors through the threaded reader and workers.
    for (unsigned iteration = 0; iteration < 128; ++iteration) {
        auto candidate = corpus[1U + random() % (corpus.size() - 1U)];
        candidate[28] ^= 1; // worker checksum failure
        auto parallel = options;
        parallel.threads = 4;
        parallel.max_in_flight = 2;
        rejects([&] { decode(candidate, parallel); });
    }
    check(rejected > 5000, "mutation campaign reached malformed paths");
    std::cout << "Mutation cases: " << accepted << " accepted, " << rejected
              << " rejected; seed 0x48554646\n";
}

void decoder_boundaries() {
    // Independently pack the complete codebook 0, 10, 110, ..., 32-bit all ones.
    // Varying symbol counts covers short tails, long-code fallback, and padding.
    for (unsigned trial = 1; trial <= 129; ++trial) {
        huffblock::EncodedBlock block{huffblock::Encoding::huffman, trial, 0, 0,
                                     std::vector<std::uint8_t>(256)};
        for (unsigned symbol = 0; symbol < 33; ++symbol) {
            block.body[symbol] = static_cast<std::uint8_t>(std::min(symbol + 1U, 32U));
        }
        std::vector<std::uint8_t> expected;
        std::uint8_t pending = 0;
        unsigned used = 0;
        for (unsigned i = 0; i < trial; ++i) {
            const unsigned symbol = (i * 17U + trial) % 33U;
            expected.push_back(static_cast<std::uint8_t>(symbol));
            const unsigned length = std::min(symbol + 1U, 32U);
            const auto code = symbol == 32 ? 0xFFFFFFFFULL : (std::uint64_t{1} << length) - 2U;
            for (unsigned remaining = length; remaining > 0; --remaining) {
                pending = static_cast<std::uint8_t>((static_cast<unsigned>(pending) << 1U) |
                                                   ((code >> (remaining - 1U)) & 1U));
                ++used;
                ++block.bit_count;
                if (used == 8) {
                    block.body.push_back(pending);
                    pending = 0;
                    used = 0;
                }
            }
        }
        if (used != 0) {
            block.body.push_back(static_cast<std::uint8_t>(static_cast<unsigned>(pending) << (8U - used)));
        }
        block.checksum = huffblock::crc32(expected);
        check(huffblock::decompress_block(block) == expected, "lookup and long-code boundary decoding");
        if (block.bit_count % 8U != 0) {
            block.body.back() |= 1;
            rejects([&] { huffblock::decompress_block(block); });
        }
    }
}

} // namespace

int main() {
    try {
        resource_limits();
        std::cout << "PASS resource and arithmetic limits\n";
        decoder_boundaries();
        std::cout << "PASS lookup, long-code, and padding boundaries\n";
        mutation_campaign();
        std::cout << "PASS deterministic mutation and truncation campaign\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL adversarial tests: " << error.what() << '\n';
        return 1;
    }
}
