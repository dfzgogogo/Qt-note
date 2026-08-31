#include "../include/serial_async_executor.hpp"

#include <cassert>
#include <chrono>
#include <future>
#include <thread>

using namespace std::chrono_literals;

int main() {
    serial_async::SerialAsyncExecutor<int> executor(1);
    int running = 0;
    std::promise<void> firstStarted;

    auto first = executor.submit(
        [&](auto control) {
            ++running;
            firstStarted.set_value();
            control.on_cancel([&] { --running; });
        },
        25ms);
    firstStarted.get_future().wait();
    auto second = executor.submit(
        [&](auto control) {
            assert(running == 0);
            control.complete(42);
        },
        100ms);
    auto full = executor.submit([](auto) {}, 100ms);

    assert(full.future().get().status == serial_async::Status::QueueFull);
    assert(first.future().get().status == serial_async::Status::Timeout);
    const auto result = second.future().get();
    assert(result.ok() && result.value == 42);

    auto cancelled = executor.submit([](auto control) {
        control.on_cancel([] {});
    }, 100ms);
    cancelled.cancel();
    assert(cancelled.future().get().status == serial_async::Status::Cancelled);

    executor.shutdown();
    auto stopped = executor.submit([](auto) {}, 100ms);
    assert(stopped.future().get().status == serial_async::Status::Stopped);
}
