# Tenco

基于 **Qt Widgets** 的机器人/底盘上位机示例工程：提供状态监控、手动遥控、MJPEG 视频流显示/录像/截图，以及地图编辑与路线分段下发。

## 功能概览

- **状态监控（HTTP 轮询）**：电量/电压/温度/运行时长/当前地图名/速度/位姿等。
- **底盘控制（WebSocket）**：发送 `cmd_vel` 速度指令；支持按钮长按连发与键盘 `W/A/S/D` 控制。
- **视频（HTTP MJPEG）**：实时显示、断线重连、录像（`.mjpeg`）与截图（`.jpg`）。
- **地图/路线**：地图网格、点/线/弧路径编辑、路线队列、分段下发与执行反馈。
- **窗口体验**：无边框窗口，支持拖动、置顶、最小化/最大化/关闭。

## 快速开始

### 环境依赖

- **Qt 5/6**（建议 Qt 6.5+）：需要模块 `core`, `gui`, `widgets`, `network`, `websockets`  
  - `Tenco.pro` 中包含 `greaterThan(QT_MAJOR_VERSION, 5): QT += core5compat`
- Windows 下可用 **MinGW** 或 **MSVC** 编译器；Qt Creator 推荐。

### 使用 Qt Creator 编译运行

1. 用 Qt Creator 打开 `Tenco.pro`
2. 选择/配置 Kit（如 Desktop Qt 6.x MinGW 64-bit）
3. Build → Run

> `Tenco.pro` 将输出目录设为 `DESTDIR = $$PWD/../bin`，构建产物通常出现在项目上级目录的 `bin/` 中。

### 命令行（可选）

确保 Qt 的 `bin` 已加入 `PATH`，然后在项目根目录执行（以 MinGW 为例）：

```bash
qmake Tenco.pro
mingw32-make -j
```

## 配置说明（config.json）

项目支持从 `config.json` 读取部分参数（见 `configmanager.cpp` 的搜索顺序）：

- 优先查找：可执行文件同级 `config.json`
- 其次：当前工作目录 `config.json`
- 再次：可执行文件上级目录 `../config.json` / `../../config.json`

`config.json` 结构示例（仓库已提供一份）：

- `geo.baseLatitudeDeg / baseLongitudeDeg`：基准经纬度（地图模块用于坐标换算）。
- `control.*`：路线跟随相关阈值与控制参数（最大速度、增益、到达判定、加减速限制等）。
- `vehicle.*`：车辆参数（轮距、轮径、减速比）。
- `video.*`：视频流 URL（MJPEG）、重连间隔、是否自动启动、是否缩放显示。

## 网络与协议（需要按你的设备调整）

当前项目内仍有部分接口地址为硬编码（见 `home.cpp`）：

- **WebSocket（底盘控制）**：`ws://192.168.31.7:1202`
- **HTTP（状态轮询）**：`http://192.168.31.7:9999/table/reads`
- **HTTP（写寄存器/模式等）**：`http://192.168.31.7:9999/table/writeIns`
- **HTTP（保存远端文件，如 GPS 配置）**：`http://192.168.31.7:9999/saveFile`

### 状态字段（轮询返回 address → UI 映射）

`home.cpp` 中对部分 `address` 的含义做了映射（以下为当前实现）：

| address | 含义（UI 显示） |
|---|---|
| `3f` | 电量百分比（Battery + `%`） |
| `38` | 电池电压（V） |
| `3c` | 模式（维护/手动/自动） |
| `100` | 车辆位姿（x, y, theta），并同步到地图模块 |
| `320` | 当前地图名称 |
| `20` | 速度显示（LCD） |
| `13` | 电池温度（℃） |
| `14` | 充放电状态 |
| `15` | 运行时长（h/m/s） |

> 具体字段语义取决于你的底盘/控制器接口协议，本项目仅按现有对接做了解析。

## 主要模块与数据流

- `MainWindow`：承载导航栏与页面容器（`QStackedWidget`），并处理无边框拖动/置顶等。
- `Home`：
  - `HomeNetworkWorker` 在独立线程周期性 HTTP 拉取状态 → 更新 UI
  - 通过 WebSocket 发送速度指令 `cmd_vel`
  - 视频流解析 JPEG 帧 → 显示/录像/截图
- `Map`：地图编辑器（`QGraphicsScene/View`），支持点/路径/路线队列；路线按段下发。
- `ConfigManager`：单例配置加载与坐标换算。

### 路线执行链路

1. `Map` 组装一段 polyline 后发出 `routeSegmentDispatched(...)`
2. `Home` 接收后进入段队列并启动跟随定时器，持续发速度
3. 段结束 `Home` 发出 `routeSegmentCompleted(bool)`
4. `Map` 收到后继续发下一段，直到队列完成或取消

## 常见问题（FAQ）

- **QDoubleSpinBox 步进按钮显示异常/点击区域错位**：在 Windows 11 风格 + 自定义 `styleSheet` 下可能出现。可尝试：
  - 在 UI 中移除该控件的 `styleSheet`（先验证）
  - 或运行时指定风格：`-style fusion` / 设置环境变量 `QT_STYLE_OVERRIDE=fusion`
- **WebSocket 报 “The proxy type is invalid for this operation”**：通常是系统代理导致。可关闭系统代理，或在代码中为 `QWebSocket` 显式设置 `NoProxy`。
