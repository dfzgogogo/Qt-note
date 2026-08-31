#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace serial_async {

enum class Status {
    Success,
    Timeout,
    Cancelled,
    QueueFull,
    Stopped,
    Failed,
};

template <typename T>
struct Result {
    Status status = Status::Failed;
    std::optional<T> value;
    std::exception_ptr error;

    [[nodiscard]] bool ok() const noexcept { return status == Status::Success; }
};

namespace detail {

template <typename T>
struct TaskState {
    std::uint64_t id;
    std::atomic<bool> cancelRequested{false};
    std::atomic<bool> completed{false};
    std::promise<Result<T>> promise;
    std::function<void(std::shared_ptr<TaskState>, Result<T>)> completionSink;
    std::function<void()> cancelHandler; // Worker-thread only.

    explicit TaskState(std::uint64_t requestId) : id(requestId) {}

    void finish(Result<T> result) {
        bool expected = false;
        if (completed.compare_exchange_strong(expected, true)) {
            promise.set_value(std::move(result));
        }
    }
};

} // namespace detail

template <typename T>
class TaskControl {
public:
    [[nodiscard]] std::uint64_t id() const noexcept { return state_->id; }

    [[nodiscard]] bool cancellation_requested() const noexcept {
        return state_->cancelRequested.load();
    }

    void complete(T value) const {
        state_->completionSink(state_, {Status::Success, std::move(value), nullptr});
    }

    void fail(std::exception_ptr error) const {
        state_->completionSink(state_, {Status::Failed, std::nullopt, std::move(error)});
    }

    // The handler runs on the executor's worker thread. It should only begin
    // cancellation; completion is owned by the executor.
    void on_cancel(std::function<void()> handler) const {
        state_->cancelHandler = std::move(handler);
    }

private:
    explicit TaskControl(std::shared_ptr<detail::TaskState<T>> state)
        : state_(std::move(state)) {}

    std::shared_ptr<detail::TaskState<T>> state_;

    template <typename>
    friend class SerialAsyncExecutor;
};

template <typename T>
class TaskHandle {
public:
    [[nodiscard]] std::uint64_t id() const noexcept { return state_->id; }

    [[nodiscard]] std::shared_future<Result<T>> future() const { return future_; }

    void cancel() const {
        if (state_->completed.load()) {
            return;
        }
        state_->cancelRequested.store(true);
        cancelRequest_(state_);
    }

    [[nodiscard]] Result<T> wait_for_or_cancel(std::chrono::milliseconds timeout) const {
        if (future_.wait_for(timeout) == std::future_status::ready) {
            return future_.get();
        }
        cancel();
        return {Status::Timeout, std::nullopt, nullptr};
    }

private:
    TaskHandle(std::shared_ptr<detail::TaskState<T>> state,
               std::shared_future<Result<T>> future,
               std::function<void(std::shared_ptr<detail::TaskState<T>>)> cancelRequest)
        : state_(std::move(state)),
          future_(std::move(future)),
          cancelRequest_(std::move(cancelRequest)) {}

    std::shared_ptr<detail::TaskState<T>> state_;
    std::shared_future<Result<T>> future_;
    std::function<void(std::shared_ptr<detail::TaskState<T>>)> cancelRequest_;

    template <typename>
    friend class SerialAsyncExecutor;
};

// Starts non-blocking operations one at a time on one standard-C++ worker
// thread. Adapters may complete TaskControl from any thread.
template <typename T>
class SerialAsyncExecutor {
public:
    using Start = std::function<void(TaskControl<T>)>;

    explicit SerialAsyncExecutor(std::size_t maxPending = 64)
        : impl_(std::make_shared<Impl>(maxPending)) {
        impl_->worker = std::thread([impl = impl_] { impl->run(); });
    }

    ~SerialAsyncExecutor() { shutdown(); }

    SerialAsyncExecutor(const SerialAsyncExecutor&) = delete;
    SerialAsyncExecutor& operator=(const SerialAsyncExecutor&) = delete;

    [[nodiscard]] TaskHandle<T> submit(Start start,
                                       std::chrono::milliseconds executionTimeout) {
        std::shared_ptr<Impl> impl;
        {
            std::lock_guard<std::mutex> lock(implMutex_);
            impl = impl_;
        }
        auto state = std::make_shared<detail::TaskState<T>>(nextRequestId_.fetch_add(1));
        auto future = state->promise.get_future().share();
        std::weak_ptr<Impl> weak = impl;
        state->completionSink = [weak](auto task, Result<T> result) {
            if (auto impl = weak.lock()) {
                impl->post([impl, task = std::move(task), result = std::move(result)]() mutable {
                    impl->completeActive(task, std::move(result));
                });
            }
        };
        auto cancel = [weak](auto task) {
            if (auto impl = weak.lock()) {
                impl->post([impl, task = std::move(task)] {
                    impl->cancelTask(task, Status::Cancelled);
                });
            }
        };
        TaskHandle<T> handle(state, std::move(future), std::move(cancel));

        if (!impl || !impl->accepting.load()) {
            state->finish({Status::Stopped, std::nullopt, nullptr});
            return handle;
        }
        {
            std::lock_guard<std::mutex> lock(impl->mutex);
            if (!impl->accepting.load()) {
                state->finish({Status::Stopped, std::nullopt, nullptr});
            } else if (impl->pending.size() >= impl->maxPending) {
                state->finish({Status::QueueFull, std::nullopt, nullptr});
            } else {
                impl->pending.push_back({std::move(state), std::move(start), executionTimeout});
                impl->wake.notify_one();
            }
        }
        return handle;
    }

    void shutdown() {
        std::shared_ptr<Impl> impl;
        {
            std::lock_guard<std::mutex> lock(implMutex_);
            impl = std::exchange(impl_, {});
        }
        if (!impl) {
            return;
        }
        impl->accepting.store(false);
        impl->post([impl] { impl->stop(); });
        if (impl->worker.joinable()) {
            impl->worker.join();
        }
    }

private:
    struct Item {
        std::shared_ptr<detail::TaskState<T>> state;
        Start start;
        std::chrono::milliseconds timeout;
    };

    struct Impl : std::enable_shared_from_this<Impl> {
        explicit Impl(std::size_t max) : maxPending(max) {}

        void post(std::function<void()> command) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                commands.push_back(std::move(command));
            }
            wake.notify_one();
        }

        void completeActive(const std::shared_ptr<detail::TaskState<T>>& task, Result<T> result) {
            if (active && active->state == task) {
                active->state->finish(std::move(result));
                active.reset();
            }
        }

        void cancelTask(const std::shared_ptr<detail::TaskState<T>>& task, Status status) {
            if (active && active->state == task) {
                finishActive(status);
                return;
            }
            for (auto it = pending.begin(); it != pending.end(); ++it) {
                if (it->state == task) {
                    it->state->finish({status, std::nullopt, nullptr});
                    pending.erase(it);
                    return;
                }
            }
        }

        void finishActive(Status status) {
            auto item = std::move(*active);
            active.reset();
            if (item.state->cancelHandler) {
                item.state->cancelHandler();
            }
            item.state->finish({status, std::nullopt, nullptr});
        }

        void stop() {
            stopping = true;
            if (active) {
                finishActive(Status::Stopped);
            }
            for (auto& item : pending) {
                item.state->finish({Status::Stopped, std::nullopt, nullptr});
            }
            pending.clear();
        }

        void run() {
            std::unique_lock<std::mutex> lock(mutex);
            while (!stopping) {
                if (!commands.empty()) {
                    auto command = std::move(commands.front());
                    commands.pop_front();
                    lock.unlock();
                    command();
                    lock.lock();
                    continue;
                }
                if (!active && !pending.empty()) {
                    active.emplace(std::move(pending.front()));
                    pending.pop_front();
                    auto start = active->start;
                    auto state = active->state;
                    deadline = std::chrono::steady_clock::now() + active->timeout;
                    lock.unlock();
                    try {
                        if (state->cancelRequested.load()) {
                            cancelTask(state, Status::Cancelled);
                        } else {
                            start(TaskControl<T>(state));
                        }
                    } catch (...) {
                        completeActive(state, {Status::Failed, std::nullopt, std::current_exception()});
                    }
                    lock.lock();
                    continue;
                }
                if (active) {
                    if (wake.wait_until(lock, deadline) == std::cv_status::timeout &&
                        commands.empty()) {
                        lock.unlock();
                        finishActive(Status::Timeout);
                        lock.lock();
                    }
                } else {
                    wake.wait(lock, [this] { return stopping || !commands.empty() || !pending.empty(); });
                }
            }
        }

        const std::size_t maxPending;
        std::atomic<bool> accepting{true};
        std::mutex mutex;
        std::condition_variable wake;
        std::deque<Item> pending;
        std::deque<std::function<void()>> commands;
        std::optional<Item> active;
        std::chrono::steady_clock::time_point deadline;
        bool stopping = false;
        std::thread worker;
    };

    std::mutex implMutex_;
    std::shared_ptr<Impl> impl_;
    std::atomic<std::uint64_t> nextRequestId_{1};
};

} // namespace serial_async
