# 通用串行异步执行器

`include/serial_async_executor.hpp` 是一个仅依赖 C++17 标准库的单线程执行器，不包含 Qt 或 Modbus 类型。它将原有设计中可复用的部分抽离出来：

- 一个 worker 线程串行启动非阻塞任务；
- 有界等待队列和 `QueueFull` 结果；
- 每个任务的执行超时；
- 从任意线程完成任务，且 completion 只会生效一次；
- 单调递增的请求 ID，迟到 completion 不会影响已开始的后续任务；
- 显式取消：排队任务立即移除；活跃任务在 worker 线程调用取消处理器；
- 关闭时拒绝新任务，并完成活跃和排队任务。

任务的 `Start` 回调在 worker 线程调用，必须快速返回。适配层应在回调中启动底层异步操作，并在完成时调用 `TaskControl::complete` 或 `fail`。`on_cancel` 注册的操作同样在 worker 线程执行，适合停止或释放底层资源。

```cpp
serial_async::SerialAsyncExecutor<std::string> executor;
auto task = executor.submit([](auto control) {
    control.on_cancel([] { /* cancel backend operation */ });
    // Later, from any thread:
    control.complete("done");
}, std::chrono::seconds(3));

auto result = task.wait_for_or_cancel(std::chrono::seconds(4));
```

`wait_for_or_cancel` 是调用方等待上限：它返回 `Timeout` 并发起异步取消。业务执行超时由执行器统一完成，避免底层任务继续占用串行 worker。底层操作必须是可取消的非阻塞操作；标准 C++ 不能安全地强制中断一个阻塞调用。
