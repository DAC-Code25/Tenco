# MJPEG VideoWorker 管线开发方案

## 1. 背景与问题

当前 `VideoClient::handleReadyRead()` 在同一个对象线程中处理多项工作：

- 从 `QNetworkReply` 读取 MJPEG 字节流。
- 在缓冲区中扫描 JPEG 起止标记。
- `QImage::loadFromData()` 解码 JPEG。
- 转换图像格式。
- 录像时把帧数据写入 `.mjpeg` 文件。
- 发出 `frameReceived(QImage)` 给首页 UI 显示。

这条链路在码流较高、磁盘较慢、UI 忙碌或连续多帧到达时容易把网络回调和 UI 刷新绑在一起。后续 OAK 本地相机后端也已经采用 worker 线程思路，因此 MJPEG 也应收敛为“网络读取轻量化、帧处理 worker 化、UI 只拿最新帧”的统一视频架构。

## 2. 本期目标

1. 新增 UI 无关的 `VideoFrameWorker` 模块。
2. `VideoClient` 继续负责 HTTP 连接、重连、状态机；MJPEG 帧解析、JPEG 解码、录像写盘交给 worker。
3. worker 只保留最新可显示帧，UI 按最大 FPS 限制接收，繁忙时允许丢帧。
4. 录像使用原始 JPEG 帧写入 `.mjpeg`，避免二次编码。
5. 提供运行指标：收到帧数、解码帧数、显示帧数、丢弃帧数、录像帧数、解码失败数、平均解码耗时、最近帧时间。
6. 补充单元测试，覆盖边界扫描、分片输入、限帧、最新帧、录像写入、指标统计。

## 3. 非目标

- 不在 V1 引入 OpenGL 渲染器。
- 不强制引入 OpenCV；OAK 后端继续在 `TENCO_ENABLE_OAK_CAMERA` 下使用 OpenCV。
- 不改变 HTTP MJPEG 协议。
- 不改变首页视频 UI 布局。
- 不把 `QNetworkAccessManager` 移到工作线程；V1 先把 CPU/磁盘重活移出网络回调。

## 4. 模块设计

新增文件：

- `videoframeworker.h`
- `videoframeworker.cpp`
- `tests/test_videoframeworker.cpp`

核心职责：

```cpp
class VideoFrameWorker : public QObject
{
    Q_OBJECT

public:
    struct Settings {
        int maxDisplayFps = 30;
        int maxBufferBytes = 3 * 1024 * 1024;
    };

    struct Metrics {
        quint64 bytesReceived = 0;
        quint64 framesReceived = 0;
        quint64 framesDecoded = 0;
        quint64 framesDisplayed = 0;
        quint64 framesDropped = 0;
        quint64 decodeFailures = 0;
        quint64 recordingFrames = 0;
        double averageDecodeMs = 0.0;
        qint64 lastFrameEpochMs = 0;
    };

    void configure(const Settings &settings);
    void reset();
    void enqueueBytes(const QByteArray &chunk);
    bool startRecording(const QString &directory, QString *outPath);
    QString stopRecording();
    bool isRecording() const;
    QImage lastFrame() const;
    Metrics metrics() const;

signals:
    void frameReady(const QImage &frame);
    void metricsUpdated(const VideoFrameWorker::Metrics &metrics);
    void workerError(const QString &message);
};
```

## 5. 最新帧与丢帧策略

1. `VideoClient` 的网络回调只读出 `QByteArray`，通过 queued connection 交给 worker。
2. worker 在自身线程中解析 MJPEG 边界，得到完整 JPEG 帧。
3. 每个完整 JPEG 都计入 `framesReceived`；解码成功计入 `framesDecoded`。
4. 若距离上次显示帧不足 `1000 / maxDisplayFps` ms，则更新 `lastFrame` 但不发 `frameReady`，计入 `framesDropped`。
5. 若 UI 来不及消费多个 `frameReady`，queued signal 仍可能排队；V1 的限帧先从源头降低信号频率，V2 可进一步加入 UI 侧 coalescing。
6. 录像不受显示限帧影响，只要 `isRecording()` 为 true，完整原始 JPEG 帧都会写入文件。

## 6. VideoClient 集成点

- 构造时创建 `QThread + VideoFrameWorker`。
- `VideoClient::handleReadyRead()` 只负责：
  - 检查 reply 状态。
  - `readAll()`。
  - `QMetaObject::invokeMethod(worker, "enqueueBytes", Qt::QueuedConnection, ...)`。
- `VideoFrameWorker::frameReady` 回到 `VideoClient` 后：
  - 更新 `m_lastFrame`。
  - 发出 `VideoClient::frameReceived`。
  - 第一次有效帧到达时把状态从 `Connecting` 切到 `Streaming`。
- `startRecording()/stopRecording()/saveSnapshot()/lastFrame()/metrics()` 通过 worker 完成或读取。

## 7. 配置策略

沿用现有 `video.previewFps` 作为 MJPEG 显示限帧参数，避免新增配置键带来迁移成本。

V1 默认：

- `maxDisplayFps = qBound(1, video.previewFps, 120)`
- `maxBufferBytes = 3 * 1024 * 1024`

后续如需要，可新增 `video.maxDecodeQueue`、`video.dropPolicy`、`video.maxDisplayLatencyMs`。

## 8. 指标与日志

V1 指标先暴露在代码接口中，供测试和后续 UI/诊断页使用：

- `VideoClient::metrics()`
- `VideoFrameWorker::metrics()`

日志建议：

- 连续解码失败只记录聚合指标，不刷屏。
- 录像写入失败通过 `workerError` 上报，`VideoClient` 转为 Error 状态并停止录像。

## 9. 测试矩阵

| 场景 | 期望 |
|---|---|
| 单个完整 JPEG chunk | 解码成功，发出 1 帧 |
| JPEG 被拆成多段输入 | 拼接后解码成功 |
| chunk 中包含多个 JPEG | 可连续解析 |
| 非法 JPEG | `decodeFailures` 增加，不发帧 |
| 高频输入 + 低显示 FPS | `framesDecoded` 增加，`framesDisplayed` 受限，`framesDropped` 增加 |
| 录像开启 | 原始 JPEG 写入 `.mjpeg`，`recordingFrames` 增加 |
| reset/stop | 清空缓冲和最新帧，关闭录像文件 |

## 10. 面试表达要点

这项改动可以表达为：把视频链路从“网络回调里同步解码和写盘”升级成“生产者/消费者式视频管线”。

加分点：

- UI 线程只处理低频最新帧，不被高码流拖住。
- 网络读取、JPEG 解析、解码、录像职责分离。
- 支持限帧和丢帧，符合实时预览“新鲜度优先”的工程原则。
- 指标可量化性能收益，便于定位卡顿和丢帧。
- 架构能与 OAK worker 后端统一，为后续 OpenCV/OpenGL 扩展留接口，而不是为了堆技术强行引入依赖。
