# Tenco

基于 **Qt Widgets** 的机器人/底盘上位机示例工程：提供状态监控、手动遥控、MJPEG 视频流显示/录像/截图，以及地图编辑、任务规划与工控机任务下发。

## 功能概览

- **状态监控（HTTP 轮询）**：电量/电压/温度/运行时长/当前地图名/速度等；融合位姿直接读取工控机。
- **底盘控制（WebSocket）**：发送 `cmd_vel` 速度指令；支持按钮长按连发与键盘 `W/A/S/D` 控制。
- **视频（HTTP MJPEG）**：实时显示、断线重连、录像（`.mjpeg`）与截图（`.jpg`）。
- **地图/路线**：地图网格、点/线/弧路径编辑、路线队列、完整任务上传与执行反馈；支持“新建/加载/另存为/保存”与未保存内容确认。
- **数据存储**：本地 SQLite 持久化为基础，预留 PostgreSQL / TimescaleDB 中心化与时序扩展方案。
- **窗口体验**：无边框窗口，支持拖动、置顶、最小化/最大化/关闭。

## 快速开始

### 环境依赖

- **Qt 6**（建议 Qt 6.5+）：需要模块 `Core`, `Gui`, `Widgets`, `Network`, `WebSockets`
- Windows 下可用 **MinGW** 或 **MSVC** 编译器；Qt Creator 推荐。

### 使用 Qt Creator 编译运行

1. 用 Qt Creator 打开项目根目录中的 `CMakeLists.txt`
2. 选择/配置 Kit（如 Desktop Qt 6.x MinGW 64-bit）
3. Build → Run

> CMake 已将运行产物统一输出到项目上级目录 `bin/`（`../bin`）。

### 命令行（可选）

确保 CMake、编译器和 Qt 工具链可用，然后在项目根目录执行：

```bash
cmake -S . -B build/cmake -G Ninja -DTENCO_BUILD_TESTS=ON
cmake --build build/cmake -j
ctest --test-dir build/cmake --output-on-failure
```

可选开关：

- `-DTENCO_ENABLE_WARNINGS=ON`：启用更严格编译告警
- `-DTENCO_BUILD_TESTS=ON`：构建单元测试目标

也可以直接使用预设：

```bash
cmake --preset default
cmake --build --preset default
ctest --preset default
```

发布构建与打包：

```powershell
cmake --preset release
cmake --build --preset release
ctest --preset release
.\scripts\package_windows.ps1
```

## 配置说明（config.json）

项目支持从 `config.json` 读取部分参数（见 `configmanager.cpp` 的搜索顺序）：

- 优先查找：可执行文件同级 `config.json`
- 其次：当前工作目录 `config.json`
- 再次：可执行文件上级目录 `../config.json` / `../../config.json`

也可通过启动参数显式指定配置文件：

```bash
./Tenco --config /path/to/config.json
```

`config.json` 结构示例（仓库已提供一份）：

- `geo.baseLatitudeDeg / baseLongitudeDeg`：基准经纬度（地图模块用于坐标换算）。
- `manualControl.*`：手动速度上限和直连心跳。
- `taskDefaults.*`：任务请求速度、停车容差和工控机安全区域 ID。
- `poseSource.*` / `tracking.*`：工控机位姿、任务服务地址和超时。
- 车辆标定由工控机统一管理，维护页只读显示当前版本。
- `video.*`：视频流 URL（MJPEG）、重连间隔、是否自动启动、是否缩放显示。

## 网络与协议（需要按你的设备调整）

接口地址可通过 `config.json` 的 `network.*` 配置（默认值见仓库自带 `config.json`）：

- `network.websocketUrl`：WebSocket（底盘控制）
- `network.statusReadUrl`：HTTP（状态轮询）
- `network.authToken`：可选，若配置则自动以 `Authorization: Bearer <token>` 访问 HTTP/WebSocket
- `network.statusPollIntervalMs`：状态轮询间隔（ms）
- `database.*`：数据库后端与连接参数。默认使用本地 SQLite，后续生产中心库建议使用 PostgreSQL；高频状态/轨迹历史可扩展到 TimescaleDB。

### 状态字段（轮询返回 address → UI 映射）

`statusprotocol.cpp` 与 `home.cpp` 中对部分 `address` 的含义做了映射（以下为当前实现）：

| address | 含义（UI 显示） |
|---|---|
| `3f` | 电量百分比（Battery + `%`） |
| `38` | 电池电压（V） |
| `3c` | 模式（维护/手动/自动） |
| 工控机 `/pose` | 权威 ENU/base_link 位姿，经坐标适配显示在地图 |
| `320` | 当前地图名称 |
| `20` | 速度显示（LCD） |
| `13` | 电池温度（℃） |
| `14` | 充放电状态 |
| `15` | 运行时长（h/m/s） |

> 具体字段语义取决于你的底盘/控制器接口协议，本项目仅按现有对接做了解析。

## 主要模块与数据流

- `MainWindow`：承载导航栏与页面容器（`QStackedWidget`），并处理无边框拖动/置顶等。
- `Home`：
  - `StatusClient`（内部使用 `HomeNetworkWorker + QThread`）：周期性 HTTP 拉取状态 → 更新 UI
  - `ChassisClient`（`QWebSocket`）：发送速度指令 `cmd_vel` / 重启 / 停止定位等
  - `VideoClient`（HTTP MJPEG）：解析 JPEG 帧 → 显示/录像/截图 + 断线重连
  - `TaskCompiler`：常规/单垄/多垄任务编译，自动速度由工控机生成。
  - `ControlSessionCoordinator`：会话、地图版本和手动/自动交接。
- `Map`：地图编辑器（`QGraphicsScene/View`），支持点/路径/路线队列；路线按段下发。
- `ConfigManager`：单例配置加载与坐标换算。

## 开发文档（更详细）

仓库内已提供更完整的“企业化”开发文档（构建/配置/架构/协议/流程/测试/CI/发布等）：

- `docs/README.md`

## 工程化增强（已落地）

- 增加应用级日志系统（`loggingmanager.*`，支持等级、分类、脱敏、会话日志、审计日志和轮转）
- 维护页新增“日志”配置页，可调整日志级别、轮转、脱敏和过滤规则
- 支持一键导出诊断包（日志、审计、脱敏配置和环境摘要）
- 状态轮询增加失败退避与请求超时保护
- WebSocket/MJPEG 重连改为指数退避，降低抖动场景重连风暴
- 地图模块增加“新建空白地图”与未保存变更保护
- CMake 增加 `tests/` 与 `ctest` 单测入口
- 增加 GitHub Actions 工作流：Windows 编译 + 测试

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
- **状态轮询日志反复出现 `http=0` / `Operation canceled` / `device not open`**：通常表示 `network.statusReadUrl` 不可达、接口响应超时，或设备未启动；当前实现对状态轮询设置了超时与失败退避，因此这类日志更接近“网络失败”而不是“地图/界面崩溃”。
  
## 界面展示
<img width="1193" height="778" alt="image" src="https://github.com/user-attachments/assets/b53abb44-51bb-4851-b58d-397784401042" />
<img width="4690" height="2933" alt="Snipaste_2025-12-31_15-15-56" src="https://github.com/user-attachments/assets/1f440180-421f-4566-86f2-067dcb48e243" />
<img width="4698" height="2935" alt="Snipaste_2025-12-31_15-16-40" src="https://github.com/user-attachments/assets/7c7df090-4116-4f6e-b258-776524c03043" />
<img width="1198" height="781" alt="image" src="https://github.com/user-attachments/assets/08882cef-4f7c-493e-a1dc-0479681a4e2d" />
<img width="1200" height="775" alt="image" src="https://github.com/user-attachments/assets/8741b1cb-2b0d-4f8c-9273-678ec1732e0e" />
<img width="1197" height="778" alt="image" src="https://github.com/user-attachments/assets/ce5777b9-2b83-40ac-b031-c01dabf94c11" />
<img width="1202" height="780" alt="image" src="https://github.com/user-attachments/assets/b629dcb8-c4bf-4302-8a47-ad8703c7aae7" />






## 工控机闭环版本

使用 [操作与接口说明](docs/工控机闭环操作与接口.md)。自动任务由工控机执行标准 Pure Pursuit；上位机无本地自动控制器和旧 row-work 网关。手动保持原 WebSocket cmd_vel 报文，无需底盘许可，直接发送。自动任务不依赖上位机运行或手动输入。旧配置会备份后迁移；旧地图/单垄/多垄文件必须确认坐标绑定后上传。本车轮距 0.25 m、左右轮径 0.20 m，现有 Lua 不修改；现场仍须核实其余标定；默认配置不证明实车已验收。
