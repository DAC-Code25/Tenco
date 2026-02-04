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

- `video.streamUrl`：MJPEG 地址（可带 `topic` 参数）
- `video.reconnectIntervalMs`：断线重连间隔
- `video.autoStart`：无话题选择控件时，是否自动启动
- `video.scaleContents`：是否 `QLabel::setScaledContents(true)`（拉伸显示）

对应模块：`VideoClient`（`videoclient.h/.cpp`）+ `Home` 的 UI 绑定（`home.cpp`）。

### 3.5 network（网络接口）

用于 HTTP/WS 的地址与轮询频率。

- `network.websocketUrl`：底盘控制 WebSocket
- `network.statusReadUrl`：状态轮询接口
- `network.writeInsUrl`：写寄存器/写模式接口
- `network.saveFileUrl`：保存远端文件接口（例如 GPS 配置）
- `network.statusPollIntervalMs`：轮询周期（ms）

对应代码：

- 读取：`ConfigManager::network()`
- 使用：`Home` 初始化 StatusClient / ChassisClient / 其它 HTTP（`home.cpp`）

---

## 4. 默认值与边界

`ConfigManager::loadDefaults()` 内有默认 URL 与轮询默认值，并对轮询间隔做了边界约束：

- 最小：50ms
- 最大：5000ms

这类“边界”属于企业项目里很重要的稳定性手段（避免误配导致 UI 卡死或网络压死）。

