#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace huffblock::detail {

// Internal pipeline shared by compression and decompression. Read owns input
// access, workers only transform owned values, and Write owns output access.
// Input/Output must be movable; Transform must support concurrent invocation.
template <class Input, class Output, class Read, class Transform, class Write>
class OrderedPipeline {
public:
    OrderedPipeline(std::size_t workers, std::size_t capacity,
                    Read read, Transform transform, Write write)
        : worker_count_(workers), capacity_(capacity), read_(std::move(read)),
          transform_(std::move(transform)), write_(std::move(write)), results_(capacity) {
        if (workers == 0 || capacity == 0) {
            throw std::invalid_argument("pipeline workers and capacity must be positive");
        }
    }
    OrderedPipeline(const OrderedPipeline&) = delete;
    OrderedPipeline& operator=(const OrderedPipeline&) = delete;
    ~OrderedPipeline() { stop_and_join(); }

    void run() {
        if (worker_count_ == 1) {
            // Preserve a genuine sequential baseline, using the same codec and
            // framing callbacks as the parallel path.
            while (auto input = read_()) {
                write_(transform_(std::move(*input), stop_.get_token()));
            }
            return;
        }
        try {
            workers_.reserve(worker_count_);
            for (std::size_t i = 0; i < worker_count_; ++i) {
                workers_.emplace_back([this] { work(); });
            }
            reader_ = std::jthread([this] { read(); });
            write();
        } catch (...) {
            fail(std::current_exception());
        }
        // Also handles partial thread creation: all previously started threads
        // are woken and joined before callbacks and state can be destroyed.
        stop_and_join();
        if (error_) {
            std::rethrow_exception(error_);
        }
    }

private:
    struct Job {
        std::size_t sequence;
        Input value;
    };

    void fail(std::exception_ptr error) {
        {
            std::lock_guard lock(mutex_);
            if (!error_) {
                error_ = error;
            }
            cancelled_ = true;
        }
        stop_.request_stop();
        changed_.notify_all();
    }

    void stop_and_join() {
        {
            std::lock_guard lock(mutex_);
            cancelled_ = true;
        }
        stop_.request_stop();
        changed_.notify_all();
        if (reader_.joinable()) {
            reader_.join();
        }
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    void read() {
        try {
            std::size_t sequence = 0;
            for (;;) {
                {
                    std::unique_lock lock(mutex_);
                    changed_.wait(lock, stop_.get_token(), [this] {
                        return cancelled_ || in_flight_ < capacity_;
                    });
                    if (cancelled_) {
                        return;
                    }
                    // Reserve BEFORE reading or allocating the input block.
                    ++in_flight_;
                }
                auto input = read_();
                {
                    std::lock_guard lock(mutex_);
                    if (cancelled_) {
                        return;
                    }
                    if (!input) {
                        --in_flight_;
                        reader_done_ = true;
                    } else {
                        if (sequence == std::numeric_limits<std::size_t>::max()) {
                            throw std::overflow_error("block sequence exhausted");
                        }
                        jobs_.push_back(Job{sequence++, std::move(*input)});
                    }
                }
                changed_.notify_all();
                if (!input) {
                    return;
                }
            }
        } catch (...) {
            fail(std::current_exception());
        }
    }

    void work() {
        try {
            for (;;) {
                std::optional<Job> job;
                {
                    std::unique_lock lock(mutex_);
                    changed_.wait(lock, stop_.get_token(), [this] {
                        return cancelled_ || !jobs_.empty() || reader_done_;
                    });
                    if (cancelled_ || jobs_.empty()) {
                        return;
                    }
                    job.emplace(std::move(jobs_.front()));
                    jobs_.pop_front();
                }
                const auto slot = job->sequence % capacity_;
                auto result = transform_(std::move(job->value), stop_.get_token());
                job.reset();
                {
                    std::lock_guard lock(mutex_);
                    if (cancelled_) {
                        return;
                    }
                    // At most capacity_ consecutive blocks are admitted, so a
                    // slot cannot be reused until its earlier result is written.
                    results_[slot].emplace(std::move(result));
                }
                changed_.notify_all();
            }
        } catch (...) {
            fail(std::current_exception());
        }
    }

    void write() {
        std::size_t sequence = 0;
        for (;;) {
            std::optional<Output> result;
            {
                std::unique_lock lock(mutex_);
                changed_.wait(lock, stop_.get_token(), [&] {
                    return cancelled_ || results_[sequence % capacity_].has_value() ||
                           (reader_done_ && in_flight_ == 0);
                });
                if (cancelled_ || (reader_done_ && in_flight_ == 0)) {
                    return;
                }
                auto& slot = results_[sequence % capacity_];
                result.emplace(std::move(*slot));
                slot.reset();
            }
            write_(std::move(*result));
            // Destroy the output buffer before releasing its admission credit.
            result.reset();
            {
                std::lock_guard lock(mutex_);
                --in_flight_;
            }
            ++sequence;
            changed_.notify_all();
        }
    }

    std::size_t worker_count_;
    std::size_t capacity_;
    Read read_;
    Transform transform_;
    Write write_;
    std::mutex mutex_;
    std::condition_variable_any changed_;
    std::stop_source stop_;
    std::exception_ptr error_;
    std::deque<Job> jobs_;
    std::vector<std::optional<Output>> results_;
    std::size_t in_flight_ = 0;
    bool reader_done_ = false;
    bool cancelled_ = false;
    // Threads are joined explicitly before any shared state is destroyed.
    std::vector<std::jthread> workers_;
    std::jthread reader_;
};

template <class Input, class Output, class Read, class Transform, class Write>
void ordered_pipeline(std::size_t workers, std::size_t capacity,
                      Read read, Transform transform, Write write) {
    OrderedPipeline<Input, Output, Read, Transform, Write> pipeline(
        workers, capacity, std::move(read), std::move(transform), std::move(write));
    pipeline.run();
}

} // namespace huffblock::detail
