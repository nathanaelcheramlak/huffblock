#include "huffblock/archive.hpp"
#include "file_output.hpp"
#include "ordered_pipeline.hpp"

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using huffblock::ExecutionOptions;
using namespace std::chrono_literals;

void check(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

template <class Function>
void rejects(Function&& function, std::string_view expected = {}) {
    try {
        function();
    } catch (const std::exception& error) {
        check(expected.empty() || std::string_view(error.what()).find(expected) != std::string_view::npos,
              "unexpected error propagated from pipeline");
        return;
    }
    throw std::runtime_error("expected failure");
}

std::string compress(const std::string& data, std::uint32_t size, ExecutionOptions options) {
    std::istringstream input(data);
    std::ostringstream output;
    huffblock::compress_stream(input, output, size, options);
    return output.str();
}

std::string decompress(const std::string& data, ExecutionOptions options) {
    std::istringstream input(data);
    std::ostringstream output;
    huffblock::decompress_stream(input, output, options);
    return output.str();
}

std::string mixed_input(std::size_t size) {
    std::mt19937 random(0x1234ABCDU);
    std::string data(size, '\0');
    for (std::size_t i = 0; i < size; ++i) {
        // Alternate compressible and raw regions, with distinct block content.
        data[i] = (i / 4096U) % 2U == 0 ? static_cast<char>('A' + (i / 4096U) % 26U) :
                                        static_cast<char>(random() & 255U);
    }
    return data;
}

void archive_matrix() {
    for (const auto& data : {std::string{}, std::string("x"), mixed_input(32768), mixed_input(32805)}) {
        for (std::uint32_t block_size : {512U, 4096U}) {
            const auto baseline = compress(data, block_size, {.threads = 1});
            for (std::uint32_t threads : {1U, 2U, 4U, 8U}) {
                for (std::uint32_t capacity : {1U, 2U, 5U, 16U}) {
                    const ExecutionOptions options{threads, capacity};
                    const auto archive = compress(data, block_size, options);
                    check(archive == baseline, "compressed bytes differ across execution settings");
                    check(decompress(archive, options) == data, "parallel round trip differs");
                }
            }
        }
    }
    const auto data = mixed_input(3U * huffblock::default_block_size + 37U);
    const auto baseline = compress(data, huffblock::default_block_size, {.threads = 1});
    check(compress(data, huffblock::default_block_size, {4, 3}) == baseline,
          "default-sized blocks preserve bytes");
    check(decompress(baseline, {4, 3}) == data, "large parallel decoding");
    check(decompress(compress("auto", 1024, {}), {}) == "auto", "automatic thread selection");
}

void bounded_reordering() {
    for (unsigned capacity : {2U, 3U, 5U}) {
        std::mutex mutex;
        std::condition_variable_any changed;
        bool release_first = false;
        std::atomic<unsigned> admitted{0};
        std::atomic<unsigned> later_completed{0};
        std::atomic<unsigned> written{0};
        std::atomic<unsigned> outstanding{0};
        std::atomic<unsigned> peak{0};
        std::atomic<bool> finished{false};
        std::exception_ptr error;
        std::vector<unsigned> output;
        unsigned next = 0;
        std::jthread runner([&] {
            try {
                huffblock::detail::ordered_pipeline<unsigned, unsigned>(8, capacity,
                    [&]() -> std::optional<unsigned> {
                        if (next == 40) {
                            return std::nullopt;
                        }
                        ++admitted;
                        const auto count = ++outstanding;
                        auto maximum = peak.load();
                        while (count > maximum && !peak.compare_exchange_weak(maximum, count)) {}
                        return next++;
                    },
                    [&](unsigned value, std::stop_token stop) {
                        if (value == 0) {
                            std::unique_lock lock(mutex);
                            changed.wait(lock, stop, [&] { return release_first; });
                        } else {
                            {
                                std::lock_guard lock(mutex);
                                ++later_completed;
                            }
                            changed.notify_all();
                        }
                        return value;
                    },
                    [&](unsigned value) {
                        output.push_back(value);
                        --outstanding;
                        ++written;
                    });
            } catch (...) {
                error = std::current_exception();
            }
            {
                std::lock_guard lock(mutex);
                finished = true;
            }
            changed.notify_all();
        });
        std::unique_lock lock(mutex);
        // GCC 11 TSan does not track pthread_cond_clockwait correctly
        // (gcc.gnu.org/bugzilla/show_bug.cgi?id=101978). A system-clock deadline
        // uses intercepted pthread_cond_timedwait without suppressing warnings.
        const bool ready = changed.wait_until(lock, std::chrono::system_clock::now() + 5s, [&] {
            return later_completed >= capacity - 1U || finished;
        });
        const auto reads_before_release = admitted.load();
        const auto writes_before_release = written.load();
        release_first = true;
        lock.unlock();
        changed.notify_all();
        runner.join();
        if (error) {
            std::rethrow_exception(error);
        }
        check(ready && reads_before_release == capacity && writes_before_release == 0,
              "slow first block must bound all later work and prevent early writes");
        check(peak <= capacity && outstanding == 0, "total admission credits remain bounded");
        check(output.size() == 40, "all results written");
        for (unsigned i = 0; i < output.size(); ++i) {
            check(output[i] == i, "out-of-order completion must preserve write order");
        }
    }
}

void pipeline_failures() {
    for (const std::string origin : {"reader", "worker", "writer"}) {
        for (unsigned workers : {1U, 2U, 4U}) {
            for (unsigned capacity : {1U, 3U}) {
                unsigned next = 0;
                std::atomic<unsigned> active{0};
                const auto message = "injected " + origin + " failure";
                rejects([&] {
                    huffblock::detail::ordered_pipeline<unsigned, unsigned>(workers, capacity,
                        [&]() -> std::optional<unsigned> {
                            if (origin == "reader" && next == 5) {
                                throw std::runtime_error(message);
                            }
                            return next == 100 ? std::nullopt : std::optional<unsigned>(next++);
                        },
                        [&](unsigned value, std::stop_token) {
                            struct Active {
                                std::atomic<unsigned>& count;
                                explicit Active(std::atomic<unsigned>& counter) : count(counter) { ++count; }
                                ~Active() { --count; }
                            } guard(active);
                            if (origin == "worker" && value == 3) {
                                throw std::runtime_error(message);
                            }
                            return value;
                        },
                        [&](unsigned value) {
                            if (origin == "writer" && value == 3) {
                                throw std::runtime_error(message);
                            }
                        });
                }, message);
                check(active == 0, "workers must finish before failure returns to caller");
            }
        }
    }
    rejects([] {
        huffblock::detail::ordered_pipeline<int, int>(4, 1,
            []() -> std::optional<int> { throw std::runtime_error("early read failure"); },
            [](int value, std::stop_token) { return value; }, [](int) {});
    }, "early read failure");
}

void cooperative_stop() {
    std::mutex mutex;
    std::condition_variable_any changed;
    bool first_waiting = false;
    std::atomic<bool> stop_observed{false};
    unsigned next = 0;
    rejects([&] {
        huffblock::detail::ordered_pipeline<unsigned, unsigned>(4, 2,
            [&]() -> std::optional<unsigned> { return next++; },
            [&](unsigned value, std::stop_token stop) {
                std::unique_lock lock(mutex);
                if (value == 0) {
                    first_waiting = true;
                    changed.notify_all();
                    changed.wait(lock, stop, [] { return false; });
                    stop_observed = stop.stop_requested();
                    return value;
                }
                if (!changed.wait_until(lock, std::chrono::system_clock::now() + 5s,
                                        [&] { return first_waiting; })) {
                    throw std::runtime_error("first worker did not start");
                }
                throw std::runtime_error("stop other worker");
            }, [](unsigned) {});
    }, "stop other worker");
    check(stop_observed, "shared cancellation token must wake a cooperative worker");
}

class FailedReadBuffer : public std::stringbuf {
public:
    explicit FailedReadBuffer(const std::string& data) : std::stringbuf(data) {}
protected:
    std::streamsize xsgetn(char* destination, std::streamsize count) override {
        if (++calls_ == 3) {
            throw std::runtime_error("injected input device failure");
        }
        return std::stringbuf::xsgetn(destination, count);
    }
private:
    unsigned calls_ = 0;
};

class FailedWriteBuffer : public std::stringbuf {
protected:
    std::streamsize xsputn(const char* source, std::streamsize count) override {
        if (++calls_ == 3) {
            return 0;
        }
        return std::stringbuf::xsputn(source, count);
    }
private:
    unsigned calls_ = 0;
};

void archive_failures() {
    const auto data = mixed_input(32768);
    const auto archive = compress(data, 1024, {.threads = 1});
    for (std::uint32_t capacity : {1U, 4U}) {
        const ExecutionOptions options{4, capacity};
        auto corrupt = archive;
        corrupt[28] ^= 1; // checksum validation happens inside a worker
        rejects([&] { decompress(corrupt, options); }, "checksum");
        rejects([&] { decompress(archive.substr(0, archive.size() - 24), options); }, "truncated");
        rejects([&] { decompress(archive + "trailing", options); }, "trailing");

        FailedReadBuffer broken_input(data);
        std::istream input(&broken_input);
        std::ostringstream sink;
        rejects([&] { huffblock::compress_stream(input, sink, 1024, options); }, "read");
        FailedWriteBuffer broken_output;
        std::ostream output(&broken_output);
        std::istringstream source(data);
        rejects([&] { huffblock::compress_stream(source, output, 1024, options); }, "write");
        FailedWriteBuffer broken_decoding_output;
        std::ostream output2(&broken_decoding_output);
        std::istringstream archive_input(archive);
        rejects([&] { huffblock::decompress_stream(archive_input, output2, options); }, "write");
    }
    rejects([&] { compress(data, 1024, {huffblock::max_worker_threads + 1, 1}); });
    rejects([&] { decompress(archive, {2, huffblock::max_in_flight_blocks + 1}); });
}

class TestDirectory {
public:
    TestDirectory() {
        const auto parent = std::filesystem::current_path();
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        path = parent / ("huffblock-parallel-test-" + std::to_string(nonce));
        check(path.parent_path() == parent && std::filesystem::create_directory(path),
              "create test directory beneath build directory");
    }
    ~TestDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    std::filesystem::path path;
};

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    check(static_cast<bool>(input), "open test file");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write_file(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.close();
    check(static_cast<bool>(output), "write test file");
}

unsigned temporary_count(const std::filesystem::path& path) {
    unsigned count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(path)) {
        if (entry.path().filename().string().starts_with(".huffblock-")) {
            ++count;
        }
    }
    return count;
}

void temporary_publication() {
    TestDirectory directory;
    const auto destination = directory.path / "output.bin";
    {
        huffblock::detail::TemporaryOutput output(destination);
        output.stream() << "complete content" << std::flush;
        check(!std::filesystem::exists(destination), "final output invisible before publication");
        check(temporary_count(directory.path) == 1, "one owned temporary directory");
        output.finish();
        check(read_file(destination) == "complete content", "published complete contents");
        check(temporary_count(directory.path) == 0, "successful publication cleans temporary files");
    }
    const auto failed = directory.path / "failed.bin";
    {
        huffblock::detail::TemporaryOutput output(failed);
        output.stream() << "partial";
        output.stream().setstate(std::ios::badbit);
        rejects([&] { output.finish(); });
    }
    check(!std::filesystem::exists(failed) && temporary_count(directory.path) == 0,
          "failed temporary output never becomes visible and is cleaned");

    const auto collision = directory.path / "late-collision.bin";
    {
        huffblock::detail::TemporaryOutput output(collision);
        output.stream() << "must not replace winner";
        write_file(collision, "winner");
        rejects([&] { output.finish(); });
    }
    check(read_file(collision) == "winner" && temporary_count(directory.path) == 0,
          "publication must preserve a destination created during processing");
    rejects([&] { huffblock::detail::TemporaryOutput existing(destination); });
#ifndef _WIN32
    const auto dangling = directory.path / "dangling";
    std::filesystem::create_symlink(directory.path / "absent-target", dangling);
    rejects([&] { huffblock::detail::TemporaryOutput link(dangling); });
    check(std::filesystem::is_symlink(dangling), "dangling symlink preserved");
#endif
}

void concurrent_publication() {
    TestDirectory directory;
    const auto destination = directory.path / "winner.bin";
    {
        huffblock::detail::TemporaryOutput a(destination);
        huffblock::detail::TemporaryOutput b(destination);
        a.stream() << "first complete file";
        b.stream() << "second complete file";
        std::barrier start(2);
        std::atomic<unsigned> successes{0};
        std::atomic<unsigned> failures{0};
        auto finish = [&](auto& output) {
            start.arrive_and_wait();
            try {
                output.finish();
                ++successes;
            } catch (const std::exception&) {
                ++failures;
            }
        };
        std::jthread first([&] { finish(a); });
        std::jthread second([&] { finish(b); });
        first.join();
        second.join();
        check(successes == 1 && failures == 1, "exactly one concurrent publisher wins");
        const auto content = read_file(destination);
        check(content == "first complete file" || content == "second complete file",
              "published file is one complete result");
    }
    check(temporary_count(directory.path) == 0, "both publishers clean owned temporary files");
}

void file_failure_cleanup() {
    TestDirectory directory;
    const auto data = mixed_input(32768);
    const auto archive = compress(data, 1024, {.threads = 1});
    const auto input = directory.path / "broken.hfb";
    const auto output = directory.path / "restored.bin";
    for (bool worker_failure : {false, true}) {
        auto broken = archive;
        if (worker_failure) {
            broken[28] ^= 1;
        } else {
            broken.resize(broken.size() - 24);
        }
        write_file(input, broken);
        rejects([&] { huffblock::decompress_file(input, output, {4, 2}); });
        check(!std::filesystem::exists(output) && temporary_count(directory.path) == 0,
              "parallel reader/worker failure leaves no final or temporary output");
        check(read_file(input) == broken, "failure preserves input archive");
    }
}

} // namespace

int main() {
    const std::vector<std::pair<std::string_view, std::function<void()>>> tests{
        {"identical archives and round trips across execution settings", archive_matrix},
        {"delayed first block bounds completed results and preserves order", bounded_reordering},
        {"reader, worker, and writer failures join all workers", pipeline_failures},
        {"shared cooperative stop token", cooperative_stop},
        {"parallel codec and I/O failures", archive_failures},
        {"temporary visibility, cleanup, and late destination collision", temporary_publication},
        {"concurrent publication never overwrites", concurrent_publication},
        {"parallel file failure cleanup", file_failure_cleanup},
    };
    unsigned failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS " << name << std::endl;
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << tests.size() - failures << '/' << tests.size() << " parallel test groups passed\n";
    return failures == 0 ? 0 : 1;
}
