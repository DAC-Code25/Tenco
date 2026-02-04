# 网络接口与协议（按当前实现整理）

提醒：协议的“权威定义”来自你的底盘/控制器文档，本页仅整理 **Tenco 当前代码里实际发送/解析的格式**，用于联调与排障。

配置入口：`config.json` 的 `network.*` 与 `video.*`（见 `docs/03_Configuration.md`）。

---

## 1. 状态轮询（HTTP POST）

### 1.1 请求（Request）

地址：`network.statusReadUrl`

请求体：JSON 数组，每个元素包含：

- `address`：寄存器/字段地址（字符串）
- `type`：类型（如 `uint8` / `float` 等）
- `len`：长度（字节数）

构造位置：`Home::Home()` 内部初始化 `StatusClient` 时（`home.cpp`）。

### 1.2 响应（Response）

`Home::handleStatusPacket()` 读取：

- 顶层：`{ "data": [...] }`
- `data[]` 元素：`{ "address": "...", "value": [...] }`

其中 `value` 是数组：

- 对于单值（电量/电压/模式等），只取 `value[0]`
- 对于位姿（address = `100`），期望 `value` 至少 3 个：`x,y,theta`

### 1.3 address 映射（当前代码）

在 `Home::handleStatusPacket()` 内做了 UI 映射，常见的有：

- `3f`：电量百分比
- `38`：电池电压
- `3c`：模式（0/1/2 → 维护/手动/自动）
- `100`：位姿（x,y,theta）并转发给地图
- `320`：地图名

注意：这些只是“当前对接协议”下的映射，换控制器可能完全不同。

---

## 2. 写寄存器/模式切换（HTTP POST）

地址：`network.writeInsUrl`

构造位置：`Home::modesubmmit()`（`home.cpp`）

请求体：JSON 数组（目前只写一个对象），形如：

- `address`: `"3c"`
- `type`: `"int8"`
- `len`: `1`
- `data`: `[<comboBox 当前索引>]`

备注：写模式前会通过 WebSocket 发送 `stopLocation`（`ChassisClient::sendStopLocation()`）。

---

## 3. 保存远端文件（GPS 配置）（HTTP POST, form-urlencoded）

地址：`network.saveFileUrl`

构造位置：`Home::orignsubmmit()`（`home.cpp`）

Content-Type：`application/x-www-form-urlencoded`

请求体（示意）：

- `path=/home/ego/User/parameter/gps.json`
- `body=<一段 JSON 文本>`

其中 `body` 的 JSON 包含经纬度、USB 参数、偏移等字段（目前大量字段是硬编码常量）。

---

## 4. 底盘控制（WebSocket）

地址：`network.websocketUrl`

发送数据格式：文本消息 JSON，形如：

```json
{
  "packet": { ... },
  "msg": { ... }
}
```

对应实现：`ChassisClient::sendJson()`（`chassisclient.cpp`）

### 4.1 速度指令 cmd_vel

- packet：`{ "cmd":"region", "region":"cmd_vel", "index":1 }`
- msg：`{ "xvel":<m/s>, "yvel":0.0, "thetavel":<rad/s>, "isRemote":true }`

### 4.2 重启

- packet：`{ "cmd":"reboot" }`

### 4.3 停止定位

- packet：`{ "cmd":"region", "region":"slam", "index":1 }`
- msg：`{ "talk":"stopLocation" }`

### 4.4 连接后“启动消息”

`ChassisClient` 在首次连上后会重复发送若干条启动消息（见 `sendStartupMessagesIfNeeded()`），用于适配当前设备的业务逻辑。

---

## 5. 视频（HTTP MJPEG）

地址：`video.streamUrl`

特点：

- 典型为 `multipart/x-mixed-replace` MJPEG 流
- `VideoClient` 通过扫描 JPEG 起止标记（FFD8/FFD9）提取帧

录像说明：

- 录像文件后缀 `.mjpeg`，本质是连续 JPEG 帧拼接
- 回放需要播放器/脚本支持（不是标准 MP4）

