# 核心架构与性能优化 V1 方案

## 目标

本轮优化聚焦六个方向：Home/Map 低风险职责拆分、CMake 模块化、网络客户端公共框架、配置 schema 校验与环境变量覆盖、路线规划参数配置化、日志异步写入。

V1 的原则是先建立可验证的工程边界，避免一次性搬空 `home.cpp` / `map.cpp` 造成高风险回归。

## 1. Home / Map 职责拆分

### Home

新增协作者：

- `HomeControlCoordinator`：负责把 `ConfigManager::ControlConfig` 转换成 `RouteFollower::ControlParams`，并生成 `MotionCommandArbiter::ManualCommandConfig`。
- `HomeVideoPresenter`：负责首页视频 QLabel 的占位文案、帧缩放、最新帧缓存和显示状态。

收益：

- `Home` 不再直接承载所有参数映射和视频显示细节。
- 后续可以继续把远程相机控制、云台控制拆成独立 coordinator。

### Map

新增协作者：

- `MapRoutePlanner`：负责把地图边转换为 `RoutePathFinder` 输入，并统一应用路线规划代价参数。

收益：

- `Map` 不再直接依赖固定路线代价参数。
- 规划逻辑可单测，后续可继续演进为 `MapRouteDispatcher`。

## 2. CMake 模块化

新增内部库：

- `tenco_config`
- `tenco_logging`
- `tenco_storage`
- `tenco_motion`
- `tenco_map_model`
- `tenco_network`
- `tenco_video`

主程序只保留 UI 编排层和入口层源文件，单测链接对应模块库，减少重复源文件列表。

## 3. 网络客户端公共框架

新增 `networkpolicy`：

- 统一 Bearer Token Header 注入。
- 统一 JSON HTTP request 构造。
- 统一指数退避计算。

首批接入：

- `HomeNetworkWorker`
- `ChassisClient`
- `RowWorkClient`

## 4. 配置 schema 校验与环境变量覆盖

新增能力：

- 配置加载时记录 schema 警告。
- 顶层 section 类型校验。
- URL 字段基础校验。
- 环境变量覆盖敏感字段：
  - `TENCO_AUTH_TOKEN`
  - `TENCO_DATABASE_PASSWORD`

这样仓库中的 `config.json` 可以保留空 token，真实部署通过环境变量注入。

## 5. 路线规划与控制参数配置化

新增 `routePlanning` 配置段：

- `minEdgeCost`
- `edgePenalty`
- `arcPenalty`

`MapRoutePlanner` 从 `ConfigManager::routePlanning()` 获取这些参数后调用 `RoutePathFinder`。

## 6. 日志异步写入

日志文件写入从 Qt message handler 中移出，改为：

```text
业务线程 / UI 线程
  -> Qt message handler 格式化、同步控制台输出
  -> 入队
  -> 后台 LogWriterThread 写文件、轮转
```

收益：

- 降低高频日志对 UI 和控制链路的阻塞。
- 保留 `shutdown()` / `exportDiagnostics()` 前的 flush 语义，确保日志可落盘。

## 测试策略

- 保留原有全量 `ctest`。
- 增加/更新配置测试：schema 警告、环境变量覆盖、路线规划参数读取。
- 增加 Map 路线规划协作者测试。
- 保持 `RouteFollower`、`RoutePathFinder`、`MotionCommandArbiter`、`VideoFrameWorker` 原有测试。

