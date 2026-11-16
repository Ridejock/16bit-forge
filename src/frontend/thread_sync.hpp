// SPDX-License-Identifier: MIT
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

namespace snes::frontend {

// Latest-value mailbox between one writer thread and one reader thread (lock-free triple
// buffer). The writer fills write_buffer() and publish()es it; the reader acquire()s the newest
// published buffer, if there is one it hasn't seen, and reads it through read_buffer(). Neither
// side ever waits for the other: a writer that is faster than the reader overwrites frames the
// reader never saw (it only ever gets the newest), and a reader that holds read_buffer() for a
// long time (a blocked present) never holds up the writer.
template <typename T>
class TripleBuffer {
public:
    // Writer thread.
    auto write_buffer() -> T& { return buffers_[back_]; }
    auto publish() -> void {
        const uint8_t previous = middle_.exchange(static_cast<uint8_t>(back_ | kFresh), std::memory_order_acq_rel);
        back_ = previous & kIndexMask;
    }

    // Reader thread. True if a newer buffer was published since the last acquire(); read_buffer()
    // then shows it. Otherwise read_buffer() still shows the last one acquired.
    auto acquire() -> bool {
        if ((middle_.load(std::memory_order_acquire) & kFresh) == 0) return false;
        const uint8_t previous = middle_.exchange(front_, std::memory_order_acq_rel);
        front_ = previous & kIndexMask;
        return true;
    }
    auto read_buffer() const -> const T& { return buffers_[front_]; }

private:
    static constexpr uint8_t kIndexMask = 3;
    static constexpr uint8_t kFresh = 4;  // the middle buffer holds a publish the reader hasn't taken

    T buffers_[3]{};
    uint8_t back_ = 0;   // writer's
    uint8_t front_ = 2;  // reader's
    std::atomic<uint8_t> middle_{1};
};

// Commands for a worker thread, run between its units of work (frames). post() queues; the
// worker drain()s. While the queue is closed (the worker isn't running: before it starts, after
// its last drain) post() refuses and the caller runs the command itself with run_inline(); the
// run mutex makes sure that never overlaps a drain.
template <typename Context>
class CommandQueue {
public:
    using Command = std::function<void(Context&)>;

    // False if closed: nothing was queued.
    auto post(Command command) -> bool {
        std::lock_guard lock(mutex_);
        if (closed_) return false;
        queue_.push_back(std::move(command));
        return true;
    }

    // Worker thread: runs everything queued so far, in order. Returns how many ran. Commands
    // posted while these run wait for the next drain.
    auto drain(Context& context) -> size_t {
        std::lock_guard run(run_mutex_);
        std::vector<Command> batch;
        {
            std::lock_guard lock(mutex_);
            batch.swap(queue_);
        }
        for (auto& command : batch) command(context);
        return batch.size();
    }

    auto run_inline(const Command& command, Context& context) -> void {
        std::lock_guard run(run_mutex_);
        command(context);
    }

    auto open() -> void {
        std::lock_guard lock(mutex_);
        closed_ = false;
    }
    // Worker thread, last thing: refuse further posts and run what is still queued.
    auto close_and_drain(Context& context) -> size_t {
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
        }
        return drain(context);
    }

private:
    std::mutex run_mutex_;  // held while commands run (drain or inline)
    std::mutex mutex_;      // guards queue_ and closed_
    std::vector<Command> queue_;
    bool closed_ = true;
};

} // namespace snes::frontend
