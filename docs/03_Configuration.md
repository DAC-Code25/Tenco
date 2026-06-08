# 配置（config.json / ConfigManager）

## 1. 为什么要配置化

企业项目里，任何“环境相关”的东西都不应写死在代码里（IP、端口、URL、阈值、速度上限、重连间隔…），否则：

- 换设备/换网络就要改代码、重新编译
- 难以排查“到底用了哪套参数”

本项目使用 `config.json` + `ConfigManager`（`configmanager.h/.cpp`）解决这个问题。

---

## 2. config.json 搜索顺序（非常重要）

见：`configmanager.cpp` 的 `defaultConfigPath()`。

加载顺序（命中即用）：

1. 可执行文件同级：`<appDir>/config.json`
2. 当前工作目录：`<cwd>/config.json`
3. 可执行文件上级：`<appDir>/../config.json`
4. 可执行文件上两级：`<appDir>/../../config.json`

你可以用 `ConfigManager::configFilePath()`（`configmanager.h`）在程序里打印“实际用了哪个配置文件”。

---

## 3. 字段说明（当前支持）

### 3.1 geo（地理基准）

用于经纬度与本地坐标换算，地图模块会读取。

- `geo.baseLatitudeDeg`
- `geo.baseLongitudeDeg`

对应代码：

- 读取：`ConfigManager::geo()`（`configmanager.h/.cpp`）
- 使用：`Map::handleModuleActivated()`（`map.cpp`）

### 3.2 control（路线跟随控制参数）

用于 `RouteFollower` 计算 `cmd_vel`（速度与角速度）。

关键字段示例：

- `maxLinearSpeed` / `maxAngularSpeed`
- `arrivalDistanceThreshold` / `arrivalAngleThresholdDeg`
- `linearGain` / `angularGain`
- `linearAccelerationLimit` / `linearDecelerationLimit`（限加减速）

对应代码：

- 读取：`ConfigManager::control()`
- 使用：`Home::Home()` 构造时设置 `RouteFollower::ControlParams`（`home.cpp`）

### 3.3 vehicle（车辆参数）

用于和车辆物理相关的参数（当前更多是预留/可扩展）。

- `wheelBaseMeters`
- `wheelDiameterMeters`
- `gearReduction`

### 3.4 video（视频流）

推荐的生产部署方式是分布式：

- 当前上位机程序运行在操作员机器
- OAK 相机通过 USB 连接在工控机
- 上位机通过 `video.streamUrl` 接工控机输出的预览流
- 上位机通过 `video.controlBaseUrl` 调用工控机相机服务的拍照/录像接口

如果控制器和工控机不是同一台设备，要明确区分这两组地址：

- `network.*`：指向底盘/控制器接口地址
- `video.streamUrl` / `video.controlBaseUrl`：指向工控机相机服务地址

当前现场环境下：

- 控制器 IP：`192.168.31.7`
- 工控机相机服务 IP：`192.168.31.13`

当前支持字段：

- `video.backend`：视频后端，当前支持 `mjpeg_http`（推荐，用于远程 MJPEG 预览）与 `oak_depthai`（仅用于本机直连调试，不是当前分布式主路径）
- `video.streamUrl`：MJPEG 地址（可带 `topic` 参数），例如工控机提供的 `/camera/stream.mjpeg`
- `video.controlBaseUrl`：工控机相机控制服务基地址，例如 `http://192.168.31.13:18080`
- `video.streamOptions`：可选的视频流列表；配置后首页 `video_topic_name` 会作为“视频流选择器”使用，每个选项直接绑定一条 MJPEG URL
- `video.deviceId`：OAK 设备 ID，多相机场景下用于绑定固定设备
- `video.previewWidth` / `video.previewHeight` / `video.previewFps`：预览参数；`previewFps` 也作为 MJPEG worker 向 UI 发帧的限帧上限
- `video.recordMode`：录像模式，当前支持 `host_opencv`
- `video.recordCodec`：录像编码，当前支持 `MJPG` / `XVID` / `MP4V`
- `video.reconnectIntervalMs`：断线重连间隔
- `video.autoStart`：无话题选择控件时，是否自动启动
- `video.scaleContents`：是否 `QLabel::setScaledContents(true)`（拉伸显示）

对应模块：

- 预览：`AbstractVideoSource` + `MjpegVideoSource`
- 远程控制：`CameraControlClient`
- 页面绑定：`Home`

`controlBaseUrl` 启用后，首页中的拍照/录像按钮会优先走远程工控机接口，而不是在上位机本地保存文件。项目侧会在启动后和运行过程中持续调用 `/camera/status`，用于同步 `cameraConnected` / `recording` 状态，并在工控机服务短时异常后自动恢复状态查询。

当前首页视频选择框支持两种模式：

- `streamOptions` 模式：推荐生产使用。选择框每一项对应一条明确的远程预览 URL，适合多相机/多码流/多工控机视频入口。
- `topic` 模板模式：兼容历史方案。当 `video.streamUrl` 自身带 `?topic=...` 查询参数时，选择框继续沿用 UI 里的历史 topic 列表，并通过替换 `topic` 参数拼接 URL。

如果项目要接入“工控机另一台相机”或者“当前相机另一条流”，必须先让工控机相机服务暴露出另一条独立的流地址，然后再写入 `streamOptions`。项目侧不会凭空生成第二路流。

当前项目侧实际依赖 `/camera/status` 的这些字段：

- `success`
- `message`
- `path`
- `cameraConnected`
- `recording`

工控机当前实际还额外返回了：

- `deviceId`
- `previewWidth`
- `previewHeight`
- `previewFps`
- `jpegQuality`
- `snapshotDir`
- `recordingDir`
- `lastPhotoPath`
- `lastRecordingPath`

推荐的分布式配置示例：

```json
"video": {
  "backend": "mjpeg_http",
  "streamUrl": "http://192.168.31.13:18080/camera/stream.mjpeg",
  "controlBaseUrl": "http://192.168.31.13:18080",
  "streamOptions": [
    {
      "name": "前置彩色相机",
      "url": "http://192.168.31.13:18080/camera/stream.mjpeg"
    },
    {
      "name": "后置相机",
      "url": "http://192.168.31.13:18080/camera/rear/stream.mjpeg"
    }
  ],
  "deviceId": "",
  "previewWidth": 1280,
  "previewHeight": 720,
  "previewFps": 30,
  "recordMode": "host_opencv",
  "recordCodec": "MJPG",
  "reconnectIntervalMs": 2000,
  "autoStart": true,
  "scaleContents": true
}
```

如果暂时只有一路远程流，也建议保留单项 `streamOptions`，这样首页行为更一致，后续扩展第二路流时不需要再改代码。

如果只是开发机上做本机直连调试，也可以使用 `oak_depthai`：

```json
"video": {
  "backend": "oak_depthai",
  "streamUrl": "",
  "controlBaseUrl": "",
  "deviceId": "",
  "previewWidth": 1280,
  "previewHeight": 720,
  "previewFps": 30,
  "recordMode": "host_opencv",
  "recordCodec": "MJPG",
  "reconnectIntervalMs": 2000,
  "autoStart": true,
  "scaleContents": true
}
```

OAK/DepthAI 的完整落地方案见 `docs/develop/usb_oak_camera_integration_plan.md`。

### 3.5 network（网络接口）

用于 HTTP/WS 的地址与轮询频率。

- `network.websocketUrl`：底盘控制 WebSocket
- `network.statusReadUrl`：状态轮询接口
- `network.writeInsUrl`：写寄存器/写模式接口
- `network.saveFileUrl`：保存远端文件接口（例如 GPS 配置）
- `network.authToken`：可选认证令牌，若非空会自动附加 `Authorization: Bearer <token>`
- `network.statusPollIntervalMs`：轮询周期（ms）

对应代码：

- 读取：`ConfigManager::network()`
- 使用：`Home` 初始化 StatusClient / ChassisClient / 其它 HTTP（`home.cpp`）

---

### 3.6 database（数据存储）

用于控制本地数据库与后续中心数据库连接。当前推荐默认使用 `sqlite`，用于单机离线可用、地图/任务/审计/状态缓存等本地持久化；生产中心化方案见 `docs/develop/database_system_plan.md`。

关键字段：

- `database.backend`：当前支持 `sqlite` / `postgresql`，默认 `sqlite`
- `database.connectionName`：Qt SQL 连接名
- `database.sqliteFilePath`：SQLite 文件路径；为空时使用 `<appDir>/data/tenco.sqlite3`
- `database.host` / `port` / `databaseName` / `userName` / `password`：PostgreSQL 连接参数
- `database.useWAL`：SQLite 是否启用 WAL
- `database.foreignKeys`：SQLite 是否启用外键约束
- `database.busyTimeoutMs`：SQLite 忙等待时间
- `database.enableTelemetryTables`：是否启用状态采样表
- `database.enableLocalCache`：是否启用本地缓存语义
- `database.enableAuditSync`：是否将审计事件同步到数据库

对应代码：

- 配置读取：`ConfigManager::database()`
- 数据库初始化：`DatabaseManager`（`databasemanager.h/.cpp`）

---

## 4. 默认值与边界

`ConfigManager::loadDefaults()` 内有默认 URL 与轮询默认值，并对轮询间隔做了边界约束：

- 最小：50ms
- 最大：5000ms

这类“边界”属于企业项目里很重要的稳定性手段（避免误配导致 UI 卡死或网络压死）。
