# Qt 异步执行器、标准库 Promise/Future 同步门面与 PImpl 隔离：可复用设计

## 1. 问题与目标

许多 Qt 通信或设备组件需要在专属 `QThread` 中使用事件驱动对象，例如：

- `QModbusClient` / `QModbusReply`；
- `QTcpSocket`、`QUdpSocket`；
- `QSerialPort`；
- `QNetworkAccessManager`；
- 自定义 `QObject` 异步设备适配器。

它们天然通过 Qt 信号槽异步完成；但上层业务通常需要简洁的、阻塞式的 C++ 调用接口，例如：

- `connect()`；
- `read()`；
- `write()`；
- `request()`；
- `shutdown()`。

本设计提供一套通用模式：

> **Qt 异步执行器（Qt Async Executor） + 标准库 Promise/Future 同步门面（Synchronous Facade） + PImpl Qt 隔离（Qt-Free Public API）**。

目标如下：

1. 公共头文件不包含 Qt 头文件、不暴露 Qt 类型；
2. 全部 Qt 对象仅在专属 Worker 线程操作；
3. 调用方通过标准库 `std::future` 获得同步等待能力；
4. 单个请求在成功、失败、超时、取消、断线和关闭时都可确定地结束；
5. 请求队列、并发提交、线程关闭和析构具有明确语义；
6. 领域逻辑可替换，执行器可复用于多种 Qt 异步后端。

该设计不试图把 Qt 变成标准库，也不让业务层感知 Qt。Qt 只负责异步执行；标准 C++ completion 契约负责跨层结果交付。

---

## 2. 设计原则

### 2.1 公共接口只表达领域，不表达框架

公共 API 应使用：

- 基础类型；
- 标准库容器与时间类型；
- 领域请求、领域结果、领域错误码；
- 可选的 `std::chrono` 超时参数。

公共 API 不应使用：

- `QObject`、`QThread`、`QString`、`QByteArray`；
- `QModbusReply`、`QNetworkReply`；
- Qt 枚举或 Qt 错误码。

原因是公共接口一旦泄漏 Qt 类型，调用方会被迫依赖 Qt 的编译、链接和线程模型；组件也难以替换 Qt 后端或进行单元测试。

### 2.2 Qt 对象必须单线程拥有和访问

`moveToThread()` 只改变 QObject 的线程亲和性；它不会让普通 C++ 成员函数调用自动切换线程。

因此：

- 任何访问 Qt I/O 对象、reply、timer、QObject 动态属性或 signal/slot 连接的操作，必须在 Worker 所属线程执行；
- 跨线程动作必须通过 queued invocation、信号槽，或仅访问纯 C++ 并发安全状态的投递入口完成；
- 外部线程不得直接读取 `QTcpSocket::state()`、调用 `disconnectDevice()`，也不得直接清理属于 Worker 线程的 Qt 对象。

### 2.3 一个请求，一个 Completion，一个终态

请求必须拥有独立的 Completion。Completion 定义请求如何结束，而不是由 Qt reply 或调用方自行决定。

Completion 的核心不变量：

- 单次请求只完成一次；
- 任一终止原因都会尝试完成它；
- 只有最先到达终态的路径获得完成权；
- 其余竞争路径只能做必要资源清理，不得改写结果；
- 调用方得到结果后，响应数据对其可见。

### 2.4 Worker 是异步资源的唯一所有者

Worker 应拥有：

- Qt 后端客户端；
- 活跃 reply；
- Qt 执行超时计时器；
- Worker 线程内的活跃请求状态；
- 连接阶段和关闭阶段的 Qt 状态。

公共门面不应保存或暴露任何可被调用方直接控制的 Qt 资源。

---

## 3. 分层与职责

```text
┌──────────────────────────────────────────────────────┐
│ 公共同步门面：Client                                  │
│ - 领域 API                                           │
│ - 参数校验、领域编码/解码、错误映射                   │
│ - 等待 future，必要时发起取消                         │
│ - 对外不包含 Qt                                       │
└───────────────────┬──────────────────────────────────┘
                    │ 纯 C++ 请求 / Completion
┌───────────────────▼──────────────────────────────────┐
│ PImpl：Client::Impl                                   │
│ - 生命周期协调                                         │
│ - 线程安全投递                                         │
│ - 对外状态快照                                         │
│ - 持有 QThread 与 Worker 的私有实现                   │
└───────────────────┬──────────────────────────────────┘
                    │ queued invocation
┌───────────────────▼──────────────────────────────────┐
│ Qt 异步执行器：Worker（专属 QThread）                 │
│ - Qt 后端对象、reply、timer                            │
│ - 串行执行 / 队列推进                                  │
│ - 信号槽适配                                           │
│ - 执行超时、取消、断线、关闭                           │
└──────────────────────────────────────────────────────┘
```

| 层 | 主要职责 | Qt 依赖 |
| --- | --- | --- |
| `Client` | 对外同步 API、领域校验、结果映射 | 无 |
| `Client::Impl` | PImpl 私有协调、状态快照、线程生命周期 | 有，但仅在 `.cpp` |
| `Request` / `Completion` | 纯 C++ 请求与结果交付协议 | 无 |
| `Worker` | Qt 异步执行、资源管理和事件适配 | 有 |
| 后端适配器 | 特定协议的 Qt 调用与 Qt 错误转换 | 有 |

领域层和 Qt 执行层之间只交换纯 C++ 数据模型。这样 Modbus、串口协议和 TCP 协议可以共用执行器框架，只替换请求构造、执行适配和领域错误映射。

---

## 4. 公共 API 的形状

公共 API 可以是同步接口，也可以额外提供非阻塞版本；两者都不暴露 Qt。

### 4.1 同步接口

```cpp
class DeviceClient {
public:
    struct Options {
        std::chrono::milliseconds requestTimeout{500};
        std::chrono::milliseconds connectTimeout{3000};
        std::size_t queueMaxSize{3};
    };

    explicit DeviceClient(Options options = {});
    ~DeviceClient();

    DeviceClient(const DeviceClient&) = delete;
    DeviceClient& operator=(const DeviceClient&) = delete;

    ConnectResult connect();
    void disconnect();

    ReadResult read(const ReadRequest& request);
    WriteResult write(const WriteRequest& request);

    ConnectionState connectionState() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
```

这里 `ConnectResult`、`ReadResult`、`WriteResult`、`ConnectionState` 都是领域类型，不包含 Qt 类型。

同步接口应明确前置条件：**不得从 Worker 所在线程调用。**否则等待 future 会阻塞 Worker 事件循环，导致 reply、计时器和 queued invocation 无法运行。

### 4.2 可选的异步接口

如果业务需要避免阻塞调用线程，可额外暴露标准库 future：

```cpp
std::future<ReadResult> readAsync(ReadRequest request);
```

同步 `read()` 可建立在 `readAsync()` 之上。这样只有一���真正的提交路径；同步版本只是等待与超时策略的薄封装。

不要在公共 API 中返回 Qt signal、`QFuture` 或 `QModbusReply*`。这会破坏 Qt 隔离边界。

---

## 5. 核心数据模型

### 5.1 内部执行结果

Worker 不应直接返回领域错误码，也不应直接暴露 Qt 错误。建议使用稳定的内部枚举：

```cpp
enum class ExecutionStatus {
    Success,
    QueueFull,
    Cancelled,
    Timeout,
    ConnectionUnavailable,
    ConnectionLost,
    ProtocolError,
    TransportError,
    InvalidRequest,
    ShuttingDown,
    InternalError
};
```

内部结果还可附带诊断数据：

```cpp
struct ExecutionResult {
    ExecutionStatus status = ExecutionStatus::InternalError;
    std::vector<std::uint16_t> values;
    int nativeErrorCode = 0;
    std::string nativeErrorText;
    std::uint64_t requestId = 0;
};
```

`nativeErrorCode` 和 `nativeErrorText` 只用于日志或内部诊断。公共层负责把 `ExecutionStatus` 映射为稳定的领域错误码。

### 5.2 Completion

Completion 应是纯 C++ 对象；它可以被调用方、队列和 Worker 安全持有。

```cpp
class Completion {
public:
    std::future<ExecutionResult> future();

    bool tryComplete(ExecutionResult result);
    bool requestCancel() noexcept;
    bool cancelRequested() const noexcept;

private:
    std::promise<ExecutionResult> promise_;
    std::atomic_bool completed_{false};
    std::atomic_bool cancelRequested_{false};
};
```

语义：

- `tryComplete()` 使用 compare-exchange 决定唯一完成者；
- `requestCancel()` 只表达调用方意图，不操作 Qt 资源；
- Worker 接到取消通知后，在 Worker 线程处理 reply、timer 和队列；
- `future()` 只能获取一次，因此应在构造或提交时提取并交给调用方。

若一个请求需要多个观察者，不应复用同一个 `std::future`；应设计共享结果状态或由上层明确管理多个等待者。

### 5.3 Request

请求应区分“领域载荷”与“执行控制信息”：

```cpp
struct PendingRequest {
    std::uint64_t id = 0;
    RequestPayload payload;
    std::shared_ptr<Completion> completion;
    std::chrono::milliseconds executionTimeout{500};
};
```

其中 `RequestPayload` 是纯 C++ 领域数据。Qt 数据单元、`QByteArray` 或 QObject 指针不应进入该结构。

`id` 是重要的竞态防护机制：超时、取消、reply 完成等事件必须只影响仍然匹配的活跃请求。

---

## 6. 执行器模型

### 6.1 线程安全提交与 Worker 线程执行分离

提交入口允许由任何调用线程执行，但它只能做纯 C++ 并发安全工作：

1. 检查组件生命周期是否仍为 `Running`；
2. 分配请求 ID；
3. 在 mutex 保护下检查队列上限并入队；
4. 队列满时立即 `tryComplete(QueueFull)`；
5. 通过 queued invocation 通知 Worker 处理队列。

Worker 线程函数负责：

1. 取出下一个未取消请求；
2. 检查连接与后端可用性；
3. 创建 Qt 请求对象并发送；
4. 保存 `activeRequestId` 与活跃 reply；
5. 启动 `QTimer` 执行超时；
6. 在回调中完成请求并继续队列。

这是一种“多线程安全提交、单线程异步执行”的模型。请求队列可以被多个调用方并发提交，但底层设备访问始终串行。

### 6.2 为什么串行是默认选择

多数面向单连接、单设备或单协议会话的 Qt 客户端更适合串行执行：

- 请求时序更容易推理；
- 活跃 reply 生命周期单一；
- 超时和取消不会彼此干扰；
- 设备协议通常不要求或不保证高并发 pipeline；
- 错误和断线处理更确定。

如果底层协议确实支持并行请求，应在执行器中显式设计活跃请求表和最大并发数；不要把串行状态机无意间扩展成并行模型。

---

## 7. 请求生命周期

### 7.1 状态机

```text
Created
  │ submit
  ▼
Queued ────── cancel ──────► Completed(Cancelled)
  │ dequeue
  ▼
Active ────── reply success ─────────────► Completed(Success)
  │
  ├────────── reply protocol/transport ──► Completed(Error)
  ├────────── execution timer ───────────► Completed(Timeout)
  ├────────── connection lost ───────────► Completed(ConnectionLost)
  ├────────── shutdown ──────────────────► Completed(ShuttingDown)
  └────────── cancel command ────────────► Completed(Cancelled)
```

每个终止边最终进入同一个“结束活跃请求”路径。该路径负责：

1. 验证请求 ID 与 `activeRequestId` 匹配；
2. 停止 Qt 计时器；
3. 断开或安全释放活跃 reply；
4. 清除活跃请求状态；
5. 在不持有队列锁时调用 `Completion::tryComplete()`；
6. 安排处理下一条请求。

### 7.2 迟到事件防护

reply 完成、超时回调、取消命令和断线通知可能在相近时刻抵达。必须假设其中某些事件会在请求已经结束后迟到。

防护规则：

- 所有事件都携带或间接验证 `RequestId`；
- 如果事件不匹配当前活跃请求，不能修改当前请求状态；
- 超时或取消后应断开 reply 的完成连接，并安排 `deleteLater()`；
- `Completion::tryComplete()` 是最后一道一次性完成保护。

请求 ID 解决“事件属于谁”的问题；一次性 Completion 解决“谁先完成”的问题。两者都需要。

---

## 8. 超时和取消协议

### 8.1 执行超时：Worker 的职责

Worker 中的 `QTimer` 是底层操作的执行超时来源。它知道如何：

- 停止计时器；
- 断开 reply 信号；
- 释放或中止 Qt reply；
- 重置活跃状态；
- 推进队列。

因此，业务意义上的 I/O 超时应由 Worker 判定为 `ExecutionStatus::Timeout`。

### 8.2 等待超时：同步门面的职责

公共同步门面使用 `future.wait_for()`，目的是防止调用方无限等待。推荐预算：

```text
同步等待上限 = Worker 执行超时 + 有限调度余量
```

调度余量用于覆盖 queued invocation、事件循环调度、完成结果回传等开销。同步门面不应与 Worker 使用完全相同的期限并把两者都当作业务超时来源。

如果外层等待先到期：

1. 通过 Completion 设置 `cancelRequested`；
2. 以 queued invocation 向 Worker 发送指定 `RequestId` 的取消命令；
3. 立即向调用方返回等待超时结果；
4. Worker 随后在线程内完成实际 I/O 资源回收。

调用方已返回并不意味着 Worker 可以跳过资源清理；同步等待与底层取消是两个阶段。

### 8.3 取消不是强制中断的承诺

某些 Qt 后端不支持立即中止所有请求。取消协议应承诺：

- 调用方不会无限等待；
- Worker 会尽快停止或忽略该请求；
- 已取消请求的迟到结果不会交付给调用方，也不会破坏下一请求；
- 队列最终会继续前进。

不要承诺底层 TCP、串口或第三方设备一定在某个精确时刻停止；这取决于后端能力。

---

## 9. 连接、断线与状态快照

### 9.1 连接也应是异步操作

连接动作不能例外。公共层不应在调用方线程写入 Worker 内部的连接 promise。

正确路径是：

1. 公共层创建连接 Completion；
2. 将“保存 Completion + 开始连接”作为一个命令 queued 到 Worker；
3. Worker 在线程内设置 Qt 连接参数、调用连接方法；
4. Qt 状态回调、同步启动失败、连接超时和 shutdown 统一结束 Completion；
5. 公共层等待 future 并映射结果。

### 9.2 连接状态查询使用原子快照

`connectionState()` 应仅读取纯 C++ 原子快照，例如：

```cpp
std::atomic<ConnectionState> connectionState_{ConnectionState::Disconnected};
```

Worker 在 Qt 状态变化回调中更新它。公共层不得为了查询状态而跨线程直接访问 Qt 客户端。

### 9.3 断线策略必须固定

对于没有自动重连的组件，推荐策略：

- 非预期断线：活跃请求立即结束为 `ConnectionLost`；
- 排队请求立即结束为 `ConnectionLost`；
- 后续请求在提交或执行前立即返回 `ConnectionUnavailable`；
- 只有新的显式 `connect()` 成功后，才允许新请求。

如果需要自动重连，应作为独立特性设计：包括退避、最大次数、队列保留策略、连接期间请求行为及关闭交互。不要把自动重连隐含在基础执行器中。

---

## 10. 生命周期与 PImpl

### 10.1 生命周期状态

`Impl` 应维护纯 C++ 生命周期状态：

```text
Running -> Stopping -> Stopped
```

含义：

| 状态 | 新请求 | Worker 行为 |
| --- | --- | --- |
| `Running` | 可提交 | 正常连接、执行与排队 |
| `Stopping` | 立即拒绝 | 取消活跃请求、结束队列、释放 Qt 资源 |
| `Stopped` | 立即拒绝 | Worker 线程已退出，不再处理请求 |

从 `Running` 到 `Stopping` 的转换必须是原子的，以防析构与并发提交同时发生。

### 10.2 关闭顺序

推荐关闭协议：

1. 原子切换到 `Stopping`，阻止新请求；
2. 如果 Worker 线程仍在运行：
   - 从非 Worker 线程，以受控 blocking queued invocation 请求 Worker 执行 shutdown；
   - 从 Worker 线程，直接执行线程内 shutdown，禁止等待自己；
3. Worker 结束活跃请求和全部排队请求为 `ShuttingDown`；
4. Worker 停止 Qt timer、释放 reply、断开设备；
5. 请求 `QThread::quit()`；
6. 调用 `QThread::wait()`，确认线程停止；
7. 销毁 Worker、线程对象与 PImpl 私有资源；
8. 标记为 `Stopped`。

任何会访问 Qt 对象的 shutdown 逻辑都必须运行在 Worker 线程。外部析构线程不能直接调用这类 Worker 方法。

### 10.3 PImpl 的价值

PImpl 在该模式中不仅是 ABI 或编译依赖优化，更是架构边界：

- 公共头文件不引入 Qt；
- `QThread`、Worker、Qt 后端和连接细节留在 `.cpp`；
- 可以替换 Qt 后端，而不改变调用方编译依赖；
- 可在测试中替换 `Impl` 所依赖的执行后端；
- 可以把同步门面稳定为一个小而明确的领域 API。

---

## 11. 错误分层

需要至少三层错误概念：

| 层 | 示例 | 用途 |
| --- | --- | --- |
| Qt 原始错误 | `QModbusDevice` 或 socket 错误 | Worker 内部诊断 |
| 执行错误 | `Timeout`、`ConnectionLost`、`ProtocolError` | 执行器与领域层之间的稳定契约 |
| 领域错误 | `ModbusError`、`DeviceError` | 公共 API 与调用方之间的契约 |

推荐映射原则：

- 队列满必须保留为 `QueueFull`；
- 生命周期关闭必须保留为 `ShuttingDown` 或对应公开错误；
- 断线必须区别于一般 `RequestFailed`；
- 协议异常应区别于传输异常；
- 领域参数错误应在投递前返回，不应进入 Qt Worker；
- 原始 Qt 错误文本进入结构化日志或诊断快照，而不是泄漏进稳定公共 API。

公共层可以返回简洁的错误码，同时提供可选诊断结构，包含请求 ID、地址、排队耗时、执行耗时和原始错误信息。

---

## 12. 同步门面的限制与替代方案

同步门面适用于：

- 业务线程调用；
- 少量、明确需要阻塞等待的设备操作；
- 希望将 Qt 依赖隐藏在组件内部的场景。

同步门面不适用于：

- Worker 线程自身；
- 需要高并发、大吞吐 pipeline 的调用路径；
- UI 主线程中长时间的阻塞 I/O。

对这些场景，应使用同一执行器之上的 `std::future` 异步接口、回调或上层任务调度，而不是嵌套 `QEventLoop`。

**不建议使用嵌套事件循环模拟同步等待。**嵌套事件循环会允许重入事件穿透当前调用栈，容易引入状态重入、关闭顺序错误和难以复现的竞态。

---

## 13. 最小实现检查清单

### 公共边界

- [ ] 公共头文件不包含 Qt 头文件。
- [ ] 公共 API 不暴露 QObject、Qt 枚举、Qt 容器或 Qt reply。
- [ ] 对外错误码是领域错误，不是 Qt 原始错误。

### 线程边界

- [ ] Qt 后端对象、reply、timer 仅在 Worker 线程访问。
- [ ] 普通成员函数不会被误认为自动跨线程调用。
- [ ] 仅线程安全投递入口可由任意线程直接调用。
- [ ] 状态查询仅访问原子快照，不直接读取 Qt 状态。

### 请求完整性

- [ ] 每个请求具有唯一 `RequestId`。
- [ ] 每个请求具有独立 Completion。
- [ ] Completion 使用结构性的一次性完成保护。
- [ ] reply、timer、取消、断线和 shutdown 均可终结请求。
- [ ] 队列满、取消和关闭不会留下未完成 future。

### 超时与关闭

- [ ] Worker 管理执行超时与 Qt 资源回收。
- [ ] 公共层等待上限触发后会通知 Worker 取消请求。
- [ ] 生命周期状态阻止关闭后的新请求。
- [ ] shutdown 在 Worker 线程执行，之后再退出和等待线程。
- [ ] 同步 API 检测并拒绝 Worker 线程内调用。

---

## 14. 推荐测试矩阵

| 场景 | 必须验证的结果 |
| --- | --- |
| 正常连接和单次请求 | 结果正确，future 完成一次 |
| 多线程并发提交 | 队列正确、底层串行、无数据竞争 |
| 队列满 | 新请求立即得到 `QueueFull` |
| 执行超时 | 活跃请求结束，timer/reply 清理，下一请求继续 |
| 外层等待先超时 | 调用方返回，Worker 收到取消并最终释放资源 |
| reply 与 timer 竞争 | 最终只完成一次 |
| 取消后 reply 迟到 | 不影响后续活跃请求 |
| 非预期断线 | 活跃与排队请求按策略结束 |
| 多调用方并发 connect | 符合已定义的 `Busy` 或共享等待语义 |
| shutdown 与提交竞争 | 新请求被拒绝，既有请求全部结束 |
| 执行中析构 | 无跨线程 Qt 操作、无永久等待、线程正常退出 |
| Worker 线程调用同步 API | 被拒绝，不发生死锁 |

测试应使用可控的假后端或本地测试服务，主动制造晚到 reply、超时、断线和关闭交错，而不是只验证正常路径。

---

## 15. 总结

这套模式的关键不是“用 `std::future` 阻塞等待 Qt”，而是建立明确的边界和归属：

- **PImpl** 负责把 Qt 从公共 API 中隔离出去；
- **Completion** 负责一次性结果交付；
- **线程安全提交入口** 负责跨线程进入队列；
- **Qt Worker** 负责全部 Qt 对象和底层 I/O 生命周期；
- **Worker 定时器** 负责执行超时；
- **同步门面** 负责调用方等待上限与取消意图；
- **请求 ID** 负责抵御迟到事件；
- **生命周期状态机** 负责关闭期间的准入控制；
- **领域层** 负责参数校验、编码解码和错误码映射。

只要这些职责不交叉，这个抽象即可稳定复用于 Qt Modbus、串口、TCP、HTTP 或任意 Qt 事件驱动设备组件，并在保持纯 C++ 公共接口的同时获得可控的线程、超时、取消和销毁语义。
