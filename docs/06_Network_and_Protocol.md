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

认证说明：

- 当 `network.authToken` 非空时，会自动附加 `Authorization: Bearer <token>` 请求头。

构造位置：

- 地址/类型表：`StatusProtocol::defaultReadRequests()`（`statusprotocol.cpp`）
- 启动配置：`Home::Home()` 初始化 `StatusClient` 时（`home.cpp`）

### 1.2 响应（Response）

`Home::handleStatusPacket()` 读取：

- 顶层：`{ "data": [...] }`
- `data[]` 元素：`{ "address": "...", "value": [...] }`

其中 `value` 是数组：

- 对于单值（电量/电压/模式等），只取 `value[0]`
- 控制位姿改由工控机 PoseClient 直读，状态寄存器不再承担定位。

### 1.3 address 映射（当前代码）

在 `Home::handleStatusPacket()` 内做了 UI 映射，常见的有：

- `3f`：电量百分比
- `38`：电池电压
- `3c`：模式（0/1/2 → 维护/手动/自动）
- `100`：已从默认轮询及地图输入删除。
- `320`：地图名

注意：这些只是“当前对接协议”下的映射，换控制器可能完全不同。

---

## 2. 模式、原点和任务控制

首页手动模式请求由独立 WebSocket 控制权通道处理；自动必须在地图任务页明确启动。已移除旧模式寄存器写入和 GPS 文件上传。原点更新、任务下发、位姿及拍摄恢复的当前接口见 [工控机闭环操作与接口](工控机闭环操作与接口.md)。

---

## 4. 底盘控制（WebSocket）

地址：`network.websocketUrl`

认证说明：

- 当 `network.authToken` 非空时，握手会附加 `Authorization: Bearer <token>`。

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

### 4.3 控制权反馈

底盘需要支持 tenco-control-v1 会话隔离、手动抢占、自动租约和锁存急停。反馈位于 controlState 包装内，必须包含实测速度、chassisBootId、ownerEpoch、owner。连接建立只订阅状态，不发送旧脚本启动或 stopLocation。详见上述接口文档。

---

## 5. 视频（HTTP MJPEG）

地址：`video.streamUrl`

特点：

- 典型为 `multipart/x-mixed-replace` MJPEG 流
- `VideoClient` 通过扫描 JPEG 起止标记（FFD8/FFD9）提取帧

录像说明：

- 录像文件后缀 `.mjpeg`，本质是连续 JPEG 帧拼接
- 回放需要播放器/脚本支持（不是标准 MP4）
