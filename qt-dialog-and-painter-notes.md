# Qt 弹窗、QDialog、QMessageBox 与 QPainter 速记

## 1. 模态弹窗与非模态弹窗

### 模态弹窗（modal）

模态弹窗打开后，用户必须先处理它，不能继续操作父窗口，直到弹窗关闭。

典型场景：
- 删除确认
- 登录窗口
- 保存/覆盖确认
- 必须填写的设置对话框

常见做法：

```cpp
QDialog dialog(this);
dialog.setModal(true);
dialog.exec();
```

特征：
- 父窗口被阻塞
- 用户只能和当前弹窗交互
- 通常通过 `exec()` 进入模态事件循环

### 非模态弹窗（modeless）

非模态弹窗打开后，不会阻塞主窗口，用户仍然可以继续操作其他窗口和界面。

典型场景：
- 查找窗口
- 日志窗口
- 调试面板
- 状态窗口

常见做法：

```cpp
QDialog dialog(this);
dialog.setModal(false);
dialog.show();
```

特征：
- 主窗口可继续交互
- 一般用 `show()`，不阻塞事件循环

### Qt 中的模态级别

Qt 还支持更细的模态类型：
- `Qt::WindowModal`：只阻塞当前窗口及其子窗口
- `Qt::ApplicationModal`：阻塞整个应用
- `Qt::NonModal`：不阻塞

## 2. QDialog 与 QMessageBox 的关系

### QDialog

`QDialog` 是 Qt 中“通用对话框”的基类，适合做：
- 自定义表单
- 设置窗口
- 新建/编辑对象的输入窗口
- 复杂交互流程

它本身很通用，通常需要你自己布局控件、添加按钮和处理信号槽。

### QMessageBox

`QMessageBox` 是 `QDialog` 的一个特殊版本，专门用于“标准消息提示”。

它适合：
- “是否删除？”
- “保存修改吗？”
- “文件不存在”
- “操作成功/失败”

本质上：

```cpp
QMessageBox : public QDialog
```

也就是说，`QMessageBox` 是一种已定义好的消息对话框，而 `QDialog` 是更底层、更通用的对话框基类。

### 区别总结

- `QDialog`：通用，适合自定义复杂 UI
- `QMessageBox`：固定风格的标准提示框，适合简单确认和通知

通常，`QMessageBox` 默认是模态的，因为它常被用来要求用户确认或处理决定。

## 3. `setToolTip` 接口介绍

`setToolTip()` 用来给控件设置悬浮提示。

例如：

```cpp
QPushButton *btn = new QPushButton("保存");
btn->setToolTip("保存当前修改并退出");
```

当鼠标停留在按钮上时，会显示一个小提示气泡，说明该控件的作用。

### 适用场景

- 按钮功能说明
- 输入框要求提示
- 图标按钮说明
- 状态显示的轻量辅助说明

### 与其他接口的区别

- `setToolTip()`：悬浮提示，小而简短
- `setStatusTip()`：状态栏提示
- `setWhatsThis()`：更详细的帮助说明

### 典型写法

```cpp
openBtn->setToolTip("打开本地文件");
openBtn->setStatusTip("打开一个已有文件");
```

## 4. `QPainter` 介绍

`QPainter` 是 Qt 中的绘图器，负责在某个目标设备上绘制内容，比如：
- `QWidget`
- `QPixmap`
- `QImage`
- `QPrinter`

它本质上可以理解为：
- 目标画布
- 画笔/画刷/字体
- 绘制命令集合

### 基本用法

```cpp
void MyWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::black);
    painter.drawLine(10, 10, 100, 100);
}
```

### 坐标系

QPainter 默认使用当前目标控件的本地坐标系：
- 原点在左上角
- x 向右增长
- y 向下增长

例如：

```cpp
painter.drawLine(width() / 2, 0, width() / 2, height());
```

这表示从控件中心顶部到控件底部画一条竖线。

### 关键状态对象

#### 画笔 `QPen`

控制线条的颜色、宽度和样式：

```cpp
painter.setPen(QPen(Qt::black, 2));
```

#### 画刷 `QBrush`

控制填充区域的颜色：

```cpp
painter.setBrush(Qt::red);
```

#### 字体 `QFont`

用于文字绘制：

```cpp
painter.setFont(QFont("Microsoft YaHei", 12));
painter.drawText(20, 40, "Hello");
```

## 5. 你这段箭头绘制代码的理解

```cpp
void ArrowBox::paintEvent(QPaintEvent *e) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    painter.setPen(QPen(Qt::black, 0.1 * width()));
    painter.drawLine(width() / 2, 0, width() / 2, 0.8 * height());

    painter.setPen(QPen(Qt::black, 1));
    painter.setBrush(Qt::black);
    painter.drawPolygon(QPolygonF()
        << QPointF(width() / 2, height())
        << QPointF(width() / 2 - 0.2 * width(), 0.8 * height())
        << QPointF(width() / 2 + 0.2 * width(), 0.8 * height()));
}
```

这段代码的绘图思想是：

1. 以控件中心为 x 坐标中心
2. 从顶部往下画一条竖线，形成箭身
3. 在底部绘制一个三角形，形成箭头头部

也就是：
- 箭身：中间竖线
- 箭头头：底部三角形

这种写法非常适合做“向下指示箭头/提示器”之类的自定义控件。

### 坐标“起点选择”

绘图时通常是从“控件尺寸 + 中心点”出发，计算关键点，而不是硬编码固定像素值。这样控件缩放时仍能保持视觉比例。

例如：
- `width() / 2`：取中心 x
- `0.8 * height()`：箭身末端位置
- `0.2 * width()`：箭头底角宽度

这是一种典型的“自适应绘制”方式。

## 6. 常用绘制接口速记

- `drawLine(x1, y1, x2, y2)`：画线
- `drawRect(x, y, w, h)`：画矩形
- `drawEllipse(x, y, w, h)`：画椭圆
- `drawPolygon(...)`：画多边形
- `drawText(x, y, text)`：画文字
- `fillRect(x, y, w, h, color)`：填充矩形
- `save()` / `restore()`：保存/恢复当前绘制状态
- `translate() / rotate() / scale()`：坐标变换
- `setRenderHint(...)`：设置渲染优化，比如抗锯齿

## 7. 一个实用经验

在 Qt 自定义绘图中，核心原则是：
- 先明确坐标系
- 再根据控件尺寸动态计算关键点
- 最后用 `QPen` / `QBrush` / `QPainter` 组织图形

这样绘制的控件更容易适配不同大小、分辨率和平台样式。

## 8. 一句话总结

- 模态/非模态决定的是��是否阻塞用户操作”
- `QDialog` 是通用对话框基类
- `QMessageBox` 是专门用于消息提示的 `QDialog` 子类
- `setToolTip()` 是悬浮提示接口
- `QPainter` 是在 QWidget 或其他设备上进行绘制的核心类，关键点在于坐标系、画笔/画刷、以及绘制函数

这个知识点在 Qt 自定义控件和 UI 交互中非常基础但也非常重要。