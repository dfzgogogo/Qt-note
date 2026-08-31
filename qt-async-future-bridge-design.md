# Qt 专属线程与标准库 Promise/Future 桥接设计

## 1. 设计目标

在 Qt 下封装网络、串口、Modbus 等事件驱动 I/O 时，内部需要使用 `QObject`、信号槽、`QThread` 和 `QTimer`；但业务调用方通常希望获得一个不依赖 Qt 类型的、同步且可超时的 C++ 接口。

本设计的目标是：

- 公共 API 只暴露领域类型、C++ 标准库类型和领域错误码；
- Qt 对象仅存在于实现层；
- Qt 异步完成事件统一转换为 `std::promise` 的一次性完成结果；
- 调用方通过 `std::future` 获得等待与超时能力；
- 所有 Qt 对象严格只在其线程亲和性所属的线程中访问；
- 支持串行请求、有限队列、执行超时、调用方放弃和组件关闭。

该模式不是以 `future` 替代 Qt 的异步机制，而是：

> 用标准 C++ completion 契约定义跨线程请求边界，并由 Qt 事件循环作为异步执行后端。

## 2. 总体结构

```text
业务线程
  │
  │  connect / read / write
  ▼
公共同步门面（不暴露 Qt）
  │  创建 Completion：promise + future + cancel state
  │
  ▼
线程安全投递入口
  │  受 mutex 保护的请求队列
  │  Queued invocation
  ▼
Qt Worker 专属线程
  │  QModbusTcpClient / QNetworkReply / QTimer ...
  │  信号槽、超时、取消、资源回收
  ▼
内部执行结果
  │
  ▼
Completion fulfill
  │
  ▼
公共门面映射为领域错误码
```

建议将其理解为六个角色：

| 角色 | 职责 | 是否依赖 Qt |
| --- | --- | --- |
| 公共同步门面 | 提供同步领域 API，等待 future 并映射公开错误码 | 否 |
| Completion | 保存 promise、future、取消状态及一次性完成保护 | 否 |
| 请求模型 | 保存领域请求参数、结果缓冲区和 Completion | 否 |
| 线程安全投递器 | 接收跨线程请求，维护队列并通知 Worker | 可选 |
| Qt Worker | 在专属线程执行 I/O、管理 reply 与计时器 | 是 |
| Qt 适配层 | 将信号、Qt 错误和 Qt 超时翻译为内部结果 | 是 |

## 3. 线程模型

### 3.1 Worker 的正确使用方式

`Worker` 移动到 `QThread` 后，通过 `Qt::QueuedConnection` 投递的槽函数，会在 Worker 所属线程的事件循环中执行。以下行为应全部发生在 Worker 线程：

- 创建、配置、使用和销毁通信 Qt 对象；
- 调用 `connectDevice()`、`disconnectDevice()`、`sendReadRequest()`、`sendWriteRequest()` 等 Qt I/O 操作；
- 管理 `QModbusReply`、`QTimer`、signal/slot 连接；
- 启动下一条排队请求；
- 对 Qt reply 的完成、超时和取消进行收敛处理。

普通 C++ 成员函数调用**不会**因为对象已经 `moveToThread()` 而自动切换线程。因此，外部线程不能直接调用会访问 Qt 对象的 Worker 方法。

### 3.2 可以跨线程直接调用的入口

允许存在少量线程安全的普通成员入口，例如 `submitRequest()`，前提是它：

- 只访问互斥锁保护的纯 C++ 队列状态；
- 不读取或操作 Qt I/O 对象；
- 只通过 queued invocation 通知 Worker 线程开始处理。

该入口应在命名、注释和访问控制上明确为“线程安全投递入口”。

### 3.3 状态查询

外部线程不应直接读取 `QModbusTcpClient::state()` 等 Qt 对象状态。

推荐做法：

- Worker 在线程内接收状态变化信号；
- Worker 更新 `std::atomic<ConnectionState>` 状态快照；
- `isConnected()` 与 `connectionState()` 仅读取该原子快照。

这样公共查询无需阻塞投递，也不会跨线程触碰 Qt 对象。

## 4. Completion：请求的一次性完成契约

每个请求都应有独立的 Completion。其语义为：

- 调用方持有 `future`；
- Worker 最终写入一个内部结果；
- 调用方超时时可声明不再关心结果；
- 成功、错误、超时、取消、断线、关闭都必须使请求进入终态；
- promise 只能 fulfill 一次。

建议的请求状态机：

```text
Queued ──► Active ──► Completed
  │          │
  │          └────► CancelRequested ──► Completed
  │
  └───────────────► Cancelled
```

`Completed` 是唯一允许完成 promise 的终态。Worker 必须防御 Qt reply 在取消或超时后迟到的 `finished` 信号，避免其污染下一条请求。

## 5. 请求执行流程

以读取寄存器为例：

1. 公共 API 校验领域参数与访问权限；
2. 创建包含 `promise`、`future`、取消标记和读取缓冲区的请求；
3. 调用线程安全投递入口；
4. 投递器将请求压入队列，并以 queued invocation 通知 Worker；
5. Worker 在线程内取出一条请求，创建 Qt 数据单元并发送请求；
6. Worker 保存活跃 reply，启动执行超时计时器；
7. reply 完成、Qt 错误或计时器超时进入同一个结束路径；
8. 结束路径清理 Qt 资源、更新队列状态、在锁外完成 promise；
9. 公共 API 从 future 得到内部结果，并映射为公开错误码；
10. Worker 继续处理下一条请求。

写请求的流程相同，只是由领域层负责把值编码为寄存器数据。

## 6. 超时与取消

### 6.1 两类超时

应明确区分以下两类超时：

| 类型 | 所在层 | 责任 |
| --- | --- | --- |
| 执行超时 | Qt Worker | 中止或释放未完成的底层 I/O，请求结束后推进队列 |
| 调用方等待上限 | 公共同步门面 | 防止调用方无限阻塞；作为防御性兜底 |

执行超时应由 Worker 中的 `QTimer` 管理，因为只有 Worker 知道如何安全地断开 reply、停止计时器和释放 Qt 资源。

### 6.2 调用方提前放弃

若 `future.wait_for()` 先返回超时：

1. 调用方将 Completion 标记为取消请求；
2. 调用方向 Worker 投递取消通知；
3. Worker 在线程内识别当前请求或队列请求；
4. 队列请求直接跳过；活跃请求断开 reply 信号、释放 reply、停止定时器；
5. Worker 完成内部状态并推进队列；
6. 对调用方已放弃的 Completion，不再尝试写入结果。

仅设置原子取消标志但不通知 Worker，会使无意义的底层请求继续运行到自身超时；这不是完整的取消闭环。

### 6.3 超时预算

不要让外层 `wait_for()` 和 Worker 的执行计时器使用完全相同的时长。应使用：

```text
外层等待上限 = 执行超时 + 调度余量
```

调度余量用于覆盖跨线程事件投递、事件循环调度和 completion 回传。业务意义上的“请求超时”仍应由 Worker 的执行超时统一判定。

## 7. 队列、锁与完成

队列应满足：

- 单个 Worker 同时只执行一条活跃请求；
- 入队时检查队列容量，满时立即完成该请求为 `QueueFull`；
- 断开连接或关闭时，当前请求和全部排队请求均必须结束；
- 已取消的队列请求在出队前跳过；
- promise 的完成操作不应发生在持有队列锁期间。

建议将结束路径拆成两个阶段：

1. **持锁阶段**：移动出待完成的 promise，重置当前请求、活跃标志和队列状态；
2. **无锁阶段**：设置 promise 结果、清理 Qt reply/计时器、继续调度。

这样可以避免在锁内执行可能触发析构、回调或复杂状态变化的操作。

## 8. 连接管理

连接请求也应使用 Completion，而不是让调用线程直接改写 Worker 的连接 promise 成员。

推荐流程：

1. 公共层创建连接 Completion；
2. 将“保存 Completion 并开始连接”作为一次 queued invocation 投递给 Worker；
3. Worker 在线程内保存连接状态，并设置 Qt 连接参数；
4. `stateChanged`、同步失败、连接超时或关闭路径统一完成连接 Completion；
5. 公共层等待 future 并得到布尔值或领域错误。

关键约束是：连接 Completion 的设置、读取、移动和清理都只发生在 Worker 线程。

## 9. 关闭与销毁

组件关闭是请求生命周期的一种终止原因，不能依赖析构时的直接成员调用来操作 Qt 对象。

推荐关闭顺序：

1. 拒绝新请求；
2. 通过 `Qt::BlockingQueuedConnection` 或等价的受控关闭协议，请求 Worker 在线程内取消活跃请求、结束队列请求、停止计时器并断开设备；
3. 让 Worker 线程退出事件循环；
4. 调用 `QThread::wait()`，确认线程已结束；
5. 在线程完全停止且无待处理事件后销毁 Worker 与其关联对象。

析构发起线程不得直接运行访问 `QModbusTcpClient`、`QTimer` 或 `QModbusReply` 的 Worker 清理逻辑。

## 10. 错误码映射

内部执行结果应尽量保留为可诊断的状态，随后由领域层映射为公开错误码。

| 内部情况 | 推荐公开结果 |
| --- | --- |
| 成功 | `None` |
| Worker 执行超时 | `Timeout` |
| Modbus 协议异常 | `SlaveError` |
| 队列满 | `QueueFull` |
| 请求被拒绝或资源冲突 | `Busy` |
| 已连接后发生链路中断 | `ConnectionLost` |
| 地址不存在 | `InvalidAddress` |
| 权限不匹配 | `AccessDenied` |
| 请求参数或数据单元非法 | `InvalidParameter` |
| 无法分类的 Qt/实现错误 | `RequestFailed` 或 `InternalError` |

不要将 `QueueFull`、`Busy` 或连接状态错误全部压缩为 `RequestFailed`，否则公共 API 虽定义了细粒度错误，调用方却无法进行有针对性的恢复。

## 11. 适用范围

该桥接模式可复用于：

- `QModbusTcpClient` 与其他 Modbus 传输；
- `QTcpSocket`、`QUdpSocket`；
- `QSerialPort`；
- `QNetworkAccessManager`；
- 自定义 Qt 协议客户端；
- 任何由 Qt 事件循环驱动、但希望提供纯 C++ 同步门面的组件。

领域请求、数据编码、错误映射可替换；Completion、队列、线程边界和关闭协议可保持不变。

## 12. 核心约束清单

- Qt 对象只在其线程亲和性所属线程访问。
- 普通成员函数调用不会自动切换到 QObject 所在线程。
- 跨线程调用通过 queued invocation、受控 blocking invocation 或线程安全纯 C++ 投递入口完成。
- 每个请求的 promise 只能完成一次。
- 每个请求在成功、失败、超时、取消和关闭时都必须结束。
- Worker 执行超时与调用方等待上限是不同层次的机制。
- 队列锁只保护队列状态；不要持锁完成 promise 或操作复杂 Qt 资源。
- 对外公开 API、领域错误和请求模型不依赖 Qt。
