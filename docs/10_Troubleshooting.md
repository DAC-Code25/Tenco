# 常见问题排查（FAQ）

## 1. moc 报错：`QNetworkReply::NetworkError has not been declared`

典型原因：头文件里使用了 `QNetworkReply::NetworkError`，但只做了前置声明，没有 `#include <QNetworkReply>`，导致 moc 生成代码时类型不完整。

本项目已修复：`videoclient.h` 已包含 `<QNetworkReply>`。

---

## 2. WebSocket 报 “The proxy type is invalid for this operation”

常见原因：系统代理导致 Qt WebSocket 行为异常。

本项目的处理：

- `ChassisClient` 对 `QWebSocket` 设置了 `QNetworkProxy::NoProxy`

如果仍有问题：

- 检查系统代理/抓包工具（如 Fiddler）是否影响

---

## 3. 状态不刷新（一直通信故障）

排查顺序：

1. 检查 `config.json` 的 `network.statusReadUrl`
2. 用 Postman/curl 直接请求接口，看是否返回 `{"data":[...]}` 结构
3. 查看 UI 日志（`Home::logMessage`）是否打印 HTTP 状态码/响应片段

如果日志里反复出现：

- `http=0`
- `error="Operation canceled"`
- `QIODevice::read ... device not open`

通常表示：`statusReadUrl` 不可达、设备没启动，或接口在客户端超时阈值内没有返回。当前实现已经对状态轮询设置请求超时和失败退避，这类日志更接近“网络失败”而不是“地图/界面崩溃”。

---

## 4. 视频不显示

排查顺序：

1. 检查 `config.json` 的 `video.streamUrl` 是否可在浏览器打开
2. 确认返回的是 MJPEG（`multipart/x-mixed-replace`）
3. 看首页日志是否提示“已连接/异常/重连”

### 视频每次约 2 秒结束，随后 2000 ms 重连

先读取同一工控机的 `/camera/status`，HTTP 200 不代表相机采集正常；检查 `success`、`cameraConnected` 和 `message`：

```powershell
curl.exe --noproxy '*' --connect-timeout 3 --max-time 5 http://192.168.31.13:18080/camera/status
curl.exe --noproxy '*' --connect-timeout 3 --max-time 8 --dump-header stream-headers.txt --output stream-body.bin --write-out 'status=%{http_code} bytes=%{size_download} seconds=%{time_total}' http://192.168.31.13:18080/camera/stream.mjpeg
```

地址须换成当前配置中的地址。流数据保存到文件，避免二进制输出到终端；持续健康的 MJPEG 请求可能到达 `--max-time` 并以 curl 28 退出，此时须结合已接收字节/帧判断，不将人为时限认定为断流。

2026-10-13 实测：状态报 `cameraConnected=false` 与 `Couldn't read data from stream: 'preview' (X_LINK_ERROR)`，独立 HTTP 三次均只返回同一张缓存 JPEG，约 2 秒正常 EOF。失败点是工控机 DepthAI/OAK 采集链。需进一步检查服务日志、USB 供电/线材/拓扑、设备复位/占用和 SDK/pipeline；X_LINK_ERROR 本身不能确定是哪一项。

`reconnectIntervalMs=2000` 是连接结束后的等待；视频首帧会重置重连计数，所以故障服务每次给缓存帧时可一直显示 attempt 1。更改重连间隔不能恢复相机采集。恢复须验证相机状态正常、帧持续更新，详见[相机断流排查记录](develop/相机断流排查开发记录.md)（DIAG-20261013-01 / P-009）。

同日 SSH 续查确认：15:29:22 内核记录 USB 运行连接断开/-19；服务采集异常处理退出线程却保留缓存，没有重建采集，HTTP 主进程仍活着使 `Restart=on-failure` 不触发。新流收到缓存后等下一帧 2 秒，再因相机断开结束响应。目标机源码为 `/home/nvidia/tenco-oak-probe`，实际服务名 `tenco-oak-camera.service`。

只读检查启动与日志：

```bash
systemctl cat tenco-oak-camera.service
systemctl status tenco-oak-camera.service --no-pager
journalctl -k -b -o short-monotonic --no-pager
tail -n 100 /opt/tenco/logs/oak_camera_service.err
```

恢复时可重启这个相机服务，再验证状态和持续新帧；本次排查未执行重启。日志按修改日期/本次事件筛选，不能把累积文件中的历史 Device crashed/端口占用当作本次证据。长期修复应加入失败后采集重建、缓存新鲜度和错误日志/健康监督；仅修改重连间隔或 `Restart=always` 无法恢复一个主进程活着、采集线程已结束的服务。

---

## 5. QDoubleSpinBox 步进按钮错位/样式异常（Windows 11 + QSS）

这是 Qt 在某些系统主题 + 自定义样式下的常见坑。

可尝试：

- 临时用 `-style fusion` 验证是否是主题问题
- 降低对 spinbox 的 QSS 定制，或统一用 Fusion 风格

---

## 6. 接口返回 401/403（认证失败）

排查顺序：

1. 检查 `config.json` 的 `network.authToken` 是否为空或过期
2. 确认服务端要求的认证方案是否为 `Bearer`（当前客户端按 Bearer 注入）
3. 先用 Postman 验证同一 token 是否可访问 `statusReadUrl/writeInsUrl/saveFileUrl/websocketUrl`

---

## 7. 路线添加时提示“点A到点B之间没有可用路径”

常见原因不是“点没有连上”，而是：

1. 中间某一段路径根本没建
2. 路径方向是反的
3. 当前路线搜索按**有向路径图**工作，不会自动反向走

排查顺序：

1. 到“路径管理”表中确认是否真的存在 `A->...->B` 的连续路径
2. 不要只看点编号和屏幕连线，要看每条路径的 `startId/endId`
3. 若 `B->A` 能加、`A->B` 不能加，通常就是路径方向建反了

---

## 8. 点击“新建”按钮时为什么有时直接提示、有时弹保存框

这是当前地图页的预期行为：

1. **没有打开本地地图，且当前就是空地图** → 提示“当前已是空地图”
2. **没有打开本地地图，但已经新建了点/路径** → 询问“是否保存当前新建地图”
3. **已经加载了本地地图文件** → 询问“是否保存当前对地图的修改”

这个逻辑是为了保护未保存的地图编辑结果，避免误操作直接清空。
