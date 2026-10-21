# Tenco 上位机

Qt 6 Widgets/C++20 上位机，负责设备状态、独立手动控制、地图/示教/规划、整项任务下发与监视，以及工控机相机预览/拍摄。自动跟踪由工控机执行；相机服务源码在并列的 [oak-camera-service](https://github.com/DAC-Code25/oak-camera-service) 仓库。

完整工作空间的唯一约束为 [根 AGENTS.md](../AGENTS.md)，架构、最近确认的状态和待办集中在 [项目说明](../docs/项目总览.md)。本仓库 README 只说明使用方法。

## 本地构建与运行

固定本机 Kit 为 Qt 6.5.3/MinGW 11.2，CMake/Ninja，Debug，`BUILD_TESTING=ON`、`TENCO_BUILD_TESTS=ON`、`-DQT_QML_DEBUG`。工作空间构建目录为 `Tenco/build/ipc-task-client-v1-Qt6.5.3-Debug`，完整配置、构建和 CTest 命令见根总约束。

Qt Creator 打开当前仓库 `CMakeLists.txt`，选择 Desktop Qt 6.5.3 MinGW 64-bit 和上述构建目录。不要复用另一个源码副本的缓存。依赖 Core、Gui、Widgets、Network、WebSockets、Sql；关于页按可用性加载 Quick/QuickWidgets/Quick3D/Quick3DAssetUtils。程序输出到工作空间 `bin/`。

已正确配置 Qt/编译器环境时，也可在仓库根使用预设进行补充验证：

```powershell
cmake --preset default
cmake --build --preset default
ctest --preset default
```

发布打包在用户明确要求时使用 release 预设及 `scripts/package_windows.ps1`。本地修改默认不提交、不推送、不创建或合并 PR，不要求 CI。

启动可显式指定配置：

```powershell
..\bin\Tenco.exe --config ..\bin\config.json
```

## 配置

未传 `--config` 时按可执行文件同级、当前目录、可执行文件上一级、上两级依次找 `config.json`；实际路径可由 ConfigManager::configFilePath 查询。`bin/config.json` 是运行配置，不因源码整理覆盖。

| 配置组 | 用途 |
|---|---|
| `manualControl` | 独立手动速度上限、发送心跳 |
| `taskDefaults` | 任务期望速度、容差、安全通道与转向区域 ID |
| `poseSource`、`tracking` | 工控机服务地址（包含 `/api/v1`）、超时及使能 |
| `geo` | 历史经纬度换算；不替代工控机 active 原点和地图绑定 |
| `routePlanning` | 最短路 minEdgeCost、edgePenalty、arcPenalty |
| `network` | 底盘 websocketUrl、statusReadUrl、认证和状态轮询 |
| `video` | 预览 streamUrl、拍摄 controlBaseUrl、streamOptions、后端及本地显示/重连参数 |
| `gimbal` | PLC 连接、状态及软限位 |
| `logging` | 日志等级、轮转、脱敏、审计和导出 |
| `database` | SQLite 或 PostgreSQL 连接、WAL、外键及缓存选项 |

schemaVersion=2；旧 schema1 的 control 迁移手动设置及原任务容差，原文件保存 `.schema1.bak`，不恢复旧跟踪器。未知版本禁止启动新的任务服务客户端。车辆标定/原点由工控机统一管理，维护页读取；行驶请求范围 0.10～1.50 m/s，任务默认 0.10/常用 0.20，单垄基础/最大默认 0.10/0.20。

`TENCO_AUTH_TOKEN` 和 `TENCO_DATABASE_PASSWORD` 可覆盖相应敏感配置。SQLite 默认 `<appDir>/data/tenco.sqlite3`；PostgreSQL/TimescaleDB 扩展不能称为已经完成的全套中心化系统。

`network.*` 指向底盘，`video.streamUrl`/`controlBaseUrl` 指向工控机相机服务，两者不能混用。生产预览用 `mjpeg_http`，本机 OAK 调试可用 `oak_depthai`。单路/多路视频通过 streamOptions 显式绑定已有 URL，配置不会创建服务端第二路视频。

配置了 controlBaseUrl 时，拍照/录像调用工控机接口，媒体留在工控机 Capture；本地视频录像后端另行配置，不能把历史拼接 MJPEG 文件当作远端 MP4。配置维护 → 视频/相机提供显式读取/应用服务端尺寸和 FPS，并回读真实生效状态；启动或本地保存不自动覆盖工控机设置。

## 操作与接口

自动任务：地图/示教 → 完整任务编译 → 上传 → 明确启动 → 工控机自主执行。重开先回读计划、校核版本和预览，再显式恢复。普通自动任务不依赖上位机在线，检查点拍照确认仍由上位机业务协调。

手动：按钮长按或 W/A/S/D → 原 WebSocket cmd_vel；松键发送本通道零速，断线清输入、重连不重放。PLC 云台为另一条 Modbus 链路，UI 应答成功不等于机械运动已经确认。

- [完整操作与控制接口](docs/工控机闭环操作与接口.md)
- [无线流量、测量方法和已知波动](docs/无线网络流量与优化说明.md)
- [相机媒体调用说明](../oak-camera-service/docs/相机媒体参数与拍摄接口调用说明.md)

状态映射由 statusprotocol.cpp 维护：3f 电量、38 电压、3c 模式、320 地图名、20 速度、13 电池温度；控制位姿独立来自工控机 `/pose/stream`，按原融合 HTTP 输出的 50 Hz 接收。

## 排查

测试进程以 `0xc0000135` 退出时，先将 Qt 和 MinGW bin 加入 PATH。状态日志 http=0、Operation canceled、device not open 时，检查 statusReadUrl、设备在线和超时；当前请求有超时及失败退避。WebSocket 代理异常时检查系统代理，ChassisClient 已使用 NoProxy。

视频异常先检查 streamUrl 的 multipart/x-mixed-replace 和相机 status 的帧新鲜度；HTTP 可达不能证明采集健康。Windows 风格与自定义 QSS 导致 SpinBox 步进显示错位时可用 `-style fusion` 核对。

GUI 冒烟入口仅测试构建启用，使用回环配置并关闭真实设备服务。云台间歇失效、拍摄恢复偶发失败和现场验收缺口集中保留在项目说明；本地/仿真通过不等于实车通过。
