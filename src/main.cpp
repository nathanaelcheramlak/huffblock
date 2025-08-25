#include "huffblock/archive.hpp"

#include <charconv>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view usage =
    "Usage:\n"
    "  huffblock compress [OPTIONS] INPUT OUTPUT\n"
    "  huffblock decompress [OPTIONS] INPUT OUTPUT\n"
    "  huffblock --help\n"
    "  huffblock --version\n\n"
    "Block size: 1..16777216 bytes; default: 2097152 (2 MiB).\n"
    "Threads: 1..256; default: detected hardware concurrency, capped at 32.\n"
    "Max in flight: 1..1024 blocks; default: twice the thread count.\n"
    "--buffer-limit BYTES: block-buffer budget; default: 268435456 (256 MiB).\n"
    "--max-output BYTES: decompression output limit; default: unlimited.\n"
    "Options: --block-size BYTES (compression only), --threads N, --max-in-flight N.\n"
    "--threads 1 runs entirely on the calling thread.\n"
    "Use -- before paths beginning with '-'. Existing output is never overwritten.\n";

class UsageError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

std::uint32_t parse_block_size(std::string_view text) {
    std::uint32_t result = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
    if (error != std::errc{} || end != text.data() + text.size() ||
        result == 0 || result > huffblock::max_block_size) {
        throw UsageError("block size must be an integer between 1 and 16777216 bytes");
    }
    return result;
}

std::uint32_t parse_count(std::string_view text, std::uint32_t limit, std::string_view name) {
    std::uint32_t result = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
    if (error != std::errc{} || end != text.data() + text.size() || result == 0 || result > limit) {
        throw UsageError(std::string(name) + " must be an integer between 1 and " + std::to_string(limit));
    }
    return result;
}

std::uint64_t parse_bytes(std::string_view text, bool allow_zero, std::string_view name) {
    std::uint64_t result = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
    if (error != std::errc{} || end != text.data() + text.size() || (!allow_zero && result == 0)) {
        throw UsageError(std::string(name) + " requires an unsigned decimal byte count");
    }
    return result;
}

std::filesystem::path utf8_path(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

int run(int argc, char* argv[]) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout << usage;
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::cout << "huffblock 0.3.0 (format version 1)\n";
        return 0;
    }
    if (argc < 2) {
        throw UsageError("a command is required");
    }
    const std::string_view command(argv[1]);
    if (command != "compress" && command != "decompress") {
        throw UsageError("unknown command: " + std::string(command));
    }
    std::uint32_t block_size = huffblock::default_block_size;
    bool size_seen = false;
    huffblock::ExecutionOptions execution;
    bool threads_seen = false;
    bool window_seen = false;
    bool buffer_seen = false;
    bool output_seen = false;
    bool options = true;
    std::vector<std::string> paths;
    for (int i = 2; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (options && argument == "--") {
            options = false;
        } else if (options && argument == "--block-size") {
            if (command != "compress" || size_seen || i + 1 >= argc) {
                throw UsageError("--block-size requires one value and is valid only for compression");
            }
            block_size = parse_block_size(argv[++i]);
            size_seen = true;
        } else if (options && (argument == "--threads" || argument == "--max-in-flight")) {
            auto& seen = argument == "--threads" ? threads_seen : window_seen;
            if (seen || i + 1 >= argc) {
                throw UsageError(std::string(argument) + " requires exactly one value");
            }
            const auto limit = argument == "--threads" ? huffblock::max_worker_threads :
                                                        huffblock::max_in_flight_blocks;
            const auto count = parse_count(argv[++i], limit, argument);
            if (argument == "--threads") {
                execution.threads = count;
            } else {
                execution.max_in_flight = count;
            }
            seen = true;
        } else if (options && (argument == "--buffer-limit" || argument == "--max-output")) {
            const bool output_limit = argument == "--max-output";
            auto& seen = output_limit ? output_seen : buffer_seen;
            if (seen || i + 1 >= argc || (output_limit && command != "decompress")) {
                throw UsageError(std::string(argument) + " requires one value; --max-output is decompression only");
            }
            const auto count = parse_bytes(argv[++i], output_limit, argument);
            if (output_limit) {
                execution.max_output_size = count;
            } else {
                execution.buffer_limit = count;
            }
            seen = true;
        } else if (options && argument.starts_with('-')) {
            throw UsageError("unknown option: " + std::string(argument));
        } else {
            paths.emplace_back(argument);
        }
    }
    if (paths.size() != 2) {
        throw UsageError("provide exactly one input path and one output path");
    }
    if (command == "compress") {
        huffblock::compress_file(utf8_path(paths[0]), utf8_path(paths[1]), block_size, execution);
    } else {
        huffblock::decompress_file(utf8_path(paths[0]), utf8_path(paths[1]), execution);
    }
    return 0;
}

} // namespace

template <class Function>
int report_errors(Function&& function) {
    try {
        return function();
    } catch (const UsageError& error) {
        std::cerr << "huffblock: " << error.what() << '\n' << usage;
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "huffblock: " << error.what() << '\n';
        return 1;
    }
}

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
    return report_errors([&] {
        std::vector<std::string> storage;
        std::vector<char*> arguments;
        storage.reserve(static_cast<std::size_t>(argc));
        arguments.reserve(static_cast<std::size_t>(argc));
        for (int i = 0; i < argc; ++i) {
            const auto utf8 = std::filesystem::path(argv[i]).u8string();
            storage.emplace_back(utf8.begin(), utf8.end());
        }
        for (auto& argument : storage) {
            arguments.push_back(argument.data());
        }
        return run(argc, arguments.data());
    });
}
#else
int main(int argc, char* argv[]) {
    return report_errors([&] { return run(argc, argv); });
}
#endif
