# Qt 生命周期与元类型使用笔记

## 1. 总体原则：优先考虑对象生命周期

Qt 中最重要的不是“用 Qt 指针还是标准库指针”，而是“谁负责对象销毁”。

核心建议：

- QObject / Qt 对象树：优先用 Qt 的对象树机制
- 普通 C++ 对象：优先用 std::unique_ptr / std::shared_ptr
- 非拥有引用：用裸指针或 QPointer
- 不要把 QObject 和 std::shared_ptr 混用
- 不要为了统一风格强行把所有指针都换成 Qt 指针

---

## 2. Qt 对象树：默认首选

QObject 支持父子对象关系，子对象会在父对象析构时自动删除。

示例：

```cpp
auto *button = new QPushButton(this);
auto *label = new QLabel(this);
```

这里 `button` 和 `label` 不需要手动 delete，因为 `this` 负责销毁它们。

适用场景：

- UI 控件
- 窗口
- 模型对象
- 组件对象

优点：

- 生命周期清晰
- 不容易泄漏
- 符合 Qt 设计哲学

---

## 3. 非拥有引用：用裸指针或 QPointer

### 裸指针
适合：

- 临时引用对象
- 只访问，不负责销毁
- 观察者/回调场景

示例：

```cpp
QWidget *w = someWidget;
```

### QPointer
适合：

- 需要“弱引用观察某个 QObject”
- 对象可能在异步过程中被销毁

示例：

```cpp
QPointer<QPushButton> button = ui->pushButton;

if (button) {
    button->setText("Done");
}
```

`QPointer` 在对象销毁后会自动失效，避免悬空指针。

---

## 4. 普通 C++ 对象：优先 std::unique_ptr

如果对象不是 QObject，最常用的方式是：

```cpp
auto worker = std::make_unique<Worker>();
```

适用场景：

- 普通堆对象
- 唯一拥有者
- 不需要 Qt 对象树

优点：

- 自动释放
- 语义清晰
- 现代 C++ 标准实践

---

## 5. 共享所有权：才用 std::shared_ptr

只有在“多个对象需要共享同一生命周期”时才考虑：

```cpp
auto doc = std::make_shared<Document>();
```

注意：

- QObject 不建议直接和 std::shared_ptr 混用
- 这会和 Qt 的 parent/child 生命周期机制冲突
- 会造成双重管理问题

---

## 6. Qt 自带智能指针：可选，不是必须

Qt 提供了：

- QScopedPointer
- QSharedPointer
- QPointer

但现代 C++ 项目中，通常更推荐：

- `std::unique_ptr`
- `std::shared_ptr`

原因：

- 标准库更通用
- 更容易和其他 C++ 代码风格统一
- Qt 生态中的智能指针通常只是补充，不是必须

---

## 7. deleteLater 的生效时机

`deleteLater()` 不是“立刻删除”，而是“延后到当前事件循环返回之后再删除”。

示例：

```cpp
widget->deleteLater();
```

它的执行顺序通常是：

1. 当前槽函数/事件处理继续执行
2. 事件处理结束
3. Qt 返回事件循环
4. Qt 在合适时机真正删除对象

优点：

- 避免在当前事件处理过程中对象被“半路销毁”
- 对 QObject/信号槽环境更安全
- 特别适合 GUI 和线程对象

适用场景：

- UI 组件销毁
- 异步任务对象
- 需要安全退出的 QObject

---

## 8. emit / signal / slot 的实现机制

### emit
`emit` 是一个宏，通常本质上相当于语法糖。

它看起来像：

```cpp
emit valueChanged(42);
```

但实际是调用 moc 生成的信号函数。

### signals
`signals:` 部分是声明，用于标记一个信号。

例如：

```cpp
signals:
    void valueChanged(int value);
```

它不是手写完整实现，而是由 moc 自动生成元对象信息。

### slots
`slots:` 是槽函数的声明，通常就是普通成员函数，只不过被 Qt 记录到了元对象系统中。

例如：

```cpp
public slots:
    void handleValue(int value) {
        qDebug() << value;
    }
```

### 真实机制
信号槽的真实运行机制依赖于：

- `Q_OBJECT`
- moc
- `connect()`
- `QMetaObject::activate()`

简化理解：

- signal = 事件发布
- slot = 事件处理函数
- connect() = 订阅关系
- moc = 自动生成元对���和函数索引
- emit = 触发事件广播

---

## 9. QVariant 与 Q_DECLARE_METATYPE

### QVariant
`QVariant` 是一个“类型擦除”的值容器，可以保存多种类型。

例如：

```cpp
QVariant v = 42;
QVariant s = QString("hello");
```

### Q_DECLARE_METATYPE
用于声明一个自定义类型是 Qt 的元类型。

例如：

```cpp
struct Person {
    QString name;
    int age;
};

Q_DECLARE_METATYPE(Person)
```

作用：

- 让类型可以放进 QVariant
- 让类型在 Qt 元对象系统中被识别
- 让自定义类型能参与某些 Qt 模板和反射机制

---

## 10. qRegisterMetaType 的作用

`qRegisterMetaType<T>()` 用于运行时注册类型。

例如：

```cpp
qRegisterMetaType<Person>("Person");
```

作用：

- 为类型提供稳定的名字
- 让 Qt 知道如何复制、析构
- 让它能够在排队连接 / 跨线程场景中安全传递

适用场景：

- `Qt::QueuedConnection`
- 跨线程信号槽
- 需要在事件队列中复制对象
- 需要完整的 metatype 运行时信息

---

## 11. `Q_DECLARE_METATYPE` 与 `qRegisterMetaType` 的关系

它们不是重复逻辑，而是两层不同的机制：

- `Q_DECLARE_METATYPE`：声明类型是 metatype，并提供按需注册入口
- `qRegisterMetaType`：真正注册到 Qt 的 metatype registry

宏的实现中，通常会在第一次需要 metatype 时执行一次 `qRegisterMetaType`，这叫“懒注册”。

也就是说：

- 只写 `Q_DECLARE_METATYPE`，通常会在第一次真正用到时自动注册
- 但显式 `qRegisterMetaType` 能保证更稳定、更早、可预测地完成注册

---

## 12. 什么时候显式调用 qRegisterMetaType

建议在以下场景中显式注册：

- 自定义类型作为信号槽参数
- 需要跨线程交互
- 使用 `Qt::QueuedConnection`
- 需要把自定义对象传入 QVariant
- 想确保它在运行时具备稳定的类型名

---

## 13. 命名空间要求

如果类型在命名空间中，推荐使用完整命名空间名：

```cpp
namespace Demo {
struct User {
    QString name;
};
}

Q_DECLARE_METATYPE(Demo::User)
qRegisterMetaType<Demo::User>("Demo::User");
```

原因：

- 避免同名类型冲突
- 保证 Qt 能正确识别唯一的类型名
- 在运行时注册时更稳定

---

## 14. QObject 包装法：可行，但不是 metatype 的标准方案

一个常见绕法是，用 QObject 包一层数据：

```cpp
class MyData : public QObject {
    Q_OBJECT
public:
    QString text;
    int id = 0;
};
```

然后传递 `QObject*` 或 `MyData*`。

这种方式可行，但它解决的是“对象语义”，不是“值语义”。

适用场景：

- 你只是想传递一个对象句柄
- 生命周期由 QObject 管理
- 不需要值复制
- 不需要 QVariant 通用存储

不推荐用于：

- 普通结构体值传递
- 跨线程排队复制
- 需要作为通用值类型被 Qt 识别

---

## 15. 决策表

### 1）QObject 生命周期

| 场景 | 建议 |
|---|---|
| UI 控件、窗口对象 | 父子对象树 |
| 观察对象，不拥有它 | QPointer / 裸指针 |
| 普通对象唯一拥有者 | std::unique_ptr |
| 多对象共享生命周期 | std::shared_ptr |

### 2）自定义类型

| 场景 | 建议 |
|---|---|
| 值语义，作为参数传递 | Q_DECLARE_METATYPE + 必要时 qRegisterMetaType |
| 对象语义 | QObject 包装 |
| QVariant 存储 | Q_DECLARE_METATYPE |
| 跨线程、queued connection | qRegisterMetaType |

### 3）信号槽

| 场景 | 建议 |
|---|---|
| 同线程直接连接 | 普通类型或 QObject |
| 跨线程连接 | 显式注册 metatype |
| 需要复制值 | Q_DECLARE_METATYPE / qRegisterMetaType |
| 只需要引用对象 | QObject* / QPointer |

---

## 16. 最短版记忆口诀

- Qt 对象树：默认
- QPointer：弱引用观察
- std::unique_ptr：普通对象默认
- std::shared_ptr：共享所有权才用
- Q_DECLARE_METATYPE：自定义值类型要被 Qt 认识
- qRegisterMetaType：跨线程/排队传递时显式注册
- QObject 包装：对象语义，不是值语义

---

## 17. 最终结论

最稳妥、最符合 Qt 习惯的方案是：

- QObject 相关：优先父子对象树
- 需要弱观察：用 QPointer
- 普通对象：用 std::unique_ptr
- 共享生命期：才用 std::shared_ptr
- 自定义值类型：Q_DECLARE_METATYPE + 必要时 qRegisterMetaType
- 对象句柄：用 QObject 包装
- `deleteLater`：用于安全延迟销毁，发生在当前事件处理结束后

这也是本次讨论中形成的核心结论。
