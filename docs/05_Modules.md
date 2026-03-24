# 主要模块与代码导读（按文件）

本页是“快速定位”的索引：你看到某个功能/问题，应该先去哪个文件看。

---

## 1. 入口与主窗口

- `main.cpp`
  - 创建 `QApplication`、设置图标、创建 `MainWindow`
- `mainwindow.h/.cpp` + `mainwindow.ui`
  - 页面容器（`QStackedWidget`）
  - 无边框拖动、置顶、最大化/最小化/关闭
  - 把 `Home` 与 `Map` 连接起来（位姿与路线分段信号）

---

## 2. 首页（Home）：业务编排中心

- `home.h/.cpp`
  - 绑定首页 UI 控件（按钮/开关/视频区域/日志框）
  - 负责把模块“串起来”，但不再把所有网络细节都堆在一个类里
  - 通过 `HomeStatusPresenter` 承接状态包解析与状态区 UI 映射
  - 关键职责：
    - StatusClient：状态轮询
    - ChassisClient：速度下发/重启/停止定位
    - VideoClient：视频显示/录像/截图
    - RouteFollower：路线段跟随（从 Map 来的 polyline）

- `home_status_presenter.h/.cpp`
  - 处理状态轮询包解析、通信状态灯更新、状态字段到 UI 的映射
  - 通过回调把位姿变化和日志回传给 `Home`

常见入口：

- `Home::handleStatusPacket()`：解析状态轮询响应并更新 UI/位姿
- `Home::applyStatusField()`：按协议字段枚举更新对应 UI
- `Home::handleKeyPress/handleKeyRelease()`：键盘遥控
- `Home::followRouteSegment()/cancelRouteExecution()`：路线执行编排

---

## 3. 状态轮询（HTTP）

- `statusclient.h/.cpp`
  - 拥有 `HomeNetworkWorker + QThread`
  - 对外暴露 `configure/start/stop`，并转发 `statusReceived/requestFailed`
- `homenetworkworker.h/.cpp`
  - 真正执行 HTTP POST 的 worker（在工作线程）
  - 维护定时器、避免重入（上一次请求未结束则延后）
- `statusprotocol.h/.cpp`
  - 状态读取地址与字段枚举映射（减少 Home 中硬编码）

---

## 4. 底盘控制（WebSocket）

- `chassisclient.h/.cpp`
  - WebSocket 连接、自动重连、NoProxy
  - 协议封装：
    - `sendVelocityCommand(xVel, thetaVel)`：`cmd_vel`
    - `sendRebootCommand()`：重启
    - `sendStopLocation()`：停止定位

---

## 5. 视频（MJPEG + 录像/截图）

- `videoclient.h/.cpp`
  - HTTP GET 拉流（multipart/x-mixed-replace）
  - 通过扫描 JPEG 起止标记（FFD8/FFD9）提取帧并解码为 `QImage`
  - 自动重连、录像（原始 JPEG 帧写入 `.mjpeg`）、截图（保存 `.jpg`）

---

## 6. 路线跟随算法（UI 无关）

- `routefollower.h/.cpp`
  - 输入：位姿（x,y,theta）+ 一段 polyline + 起止朝向约束
  - 输出：`velocityCommand(linear, angular)`（不做网络发送）
  - 具备限速、限加减速、近点减速、起始/末端朝向校准等策略

这块非常适合写单元测试（见 `docs/08_Testing_and_CI.md`）。

---

## 7. 路径规划（UI 无关）

- `routepathfinder.h/.cpp`
  - 路径图最短路搜索（加权 Dijkstra）
  - 代价包含距离与路径类型惩罚（如弧线惩罚）

---

## 8. 地图编辑与路线队列

- `map.h/.cpp`
  - QGraphicsScene 地图编辑器：点、路径（直线/圆弧）、路线队列
  - 地图文件：JSON 序列化/反序列化（内部调用 `mapdocument.*`）
  - 路线分段派发：通过信号 `routeSegmentDispatched(...)` 交给 Home 执行
- `mapgraphicsview.h/.cpp`
  - 视图交互：缩放、平移、鼠标点击映射到场景坐标

- `mapdocument.h/.cpp`
  - 地图文档模型与 JSON 编解码（可独立单测）

---

## 9. 配置与坐标换算

- `configmanager.h/.cpp`
  - 读取 `config.json`（带默认值与边界限制）
  - 提供地理坐标与本地坐标换算：`geoToLocal()/localToGeo()`

---

## 10. 自定义控件与样式

- `battery.h/.cpp`：自定义电池控件（`Q_PROPERTY` 可在 Designer 配置）
- `imageswitch.h/.cpp`：图片开关控件
- `UI_Design.cpp`：集中设置控件样式（QSS 字符串）

企业项目里更常见的做法：把样式抽到 `.qss` 文件或资源中统一管理（见 `docs/11_Roadmap.md`）。
