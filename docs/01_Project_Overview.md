# 项目概览（Tenco 是什么）

## 1. 项目定位

**Tenco** 是一个基于 **Qt Widgets** 的机器人/底盘上位机示例工程，主要用于：

- 通过 HTTP 轮询显示底盘/控制器状态
- 通过 WebSocket 下发速度指令（`cmd_vel`）与控制指令（如重启/停止定位）
- 通过 HTTP MJPEG 拉流显示视频，并支持录像与截图
- 提供地图编辑与“路线分段下发/执行反馈”的基本闭环

入口：`main.cpp` → `MainWindow`（`mainwindow.h/.cpp` + `mainwindow.ui`）。

---

## 2. 功能边界（当前已做 vs 未做）

### 已做（以仓库代码为准）

- 首页：状态显示、手动遥控（按钮 + 键盘）、重启/模式切换/保存 GPS 配置、视频显示/录像/截图
- 地图：网格、点/路径（直线/圆弧）、路线队列、分段派发与执行控制（开始/暂停/继续/停止）、地图文件保存/加载（JSON），以及“新建空白地图 + 未保存内容确认”

### 未做/不确定（取决于你的设备与协议）

- 协议字段的权威定义（哪些 address 对应什么含义）通常来自底盘/控制器文档
- 自动驾驶完整闭环（上位机只是下发与监控，真正的闭环控制可能在底盘侧）
- 完整异常处理（例如：网络波动时路线暂停/恢复策略、视频码流异常、接口返回格式变化等）
- 路线的可达性判断基于**有向路径图**；视觉上点线连通，不代表任意起终点都可直接添加到路线队列

---

## 3. 运行环境与外部依赖

- Qt：建议 Qt 6.5+（本项目统一使用 CMake）
- 模块依赖：`core/gui/widgets/network/websockets`
- 设备侧依赖（必须按你的设备修改 `config.json`）：
  - `network.statusReadUrl`：状态轮询 HTTP 接口
  - `network.websocketUrl`：控制 WebSocket
  - `video.streamUrl`：MJPEG 拉流 URL（通常带 `topic` 查询参数）

---

## 4. 你最需要先搞清楚的 3 件事（学习路径）

1. `config.json` 里每个 URL 指向哪个服务、返回什么（见 `docs/06_Network_and_Protocol.md`）
2. 首页闭环链路：状态轮询更新 UI；手动控制下发 `cmd_vel`；视频拉流显示（见 `docs/04_Architecture.md`）
3. 地图路线闭环链路：Map → TaskCompiler → TrackingClient → 工控机 → Map（见 `docs/04_Architecture.md`）
