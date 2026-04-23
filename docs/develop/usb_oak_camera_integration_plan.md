# Tenco USB OAK Camera Integration Plan

## 1. 目标与范围

本文档给出一套面向生产可维护性的 USB 相机接入方案，用于将 `OAK-D-Pro-W` 通过 USB 连接到 `11F1E2` 工控机，在 `Tenco` 首页视频区域中实现：

- 实时显示相机画面
- 拍照并保存图片
- 录像并保存视频
- 与当前项目现有的多条通信链路并存，不引入明显的无线带宽竞争与系统稳定性风险

本文档包含两版实现：

- **V1 稳定落地版**：`工控机侧 DepthAI C++ + OpenCV + HTTP 相机服务` + `上位机侧 Qt + MJPEG 预览 + 远程控制`
- **V2 高性能增强版**：`DepthAI 设备端编码 + 工控机视频服务优化 + 可选 OpenGL / 远程子码流`

同时覆盖两部分内容：

- **上位机项目（当前仓库 Tenco）如何改造**
- **工控机（11F1E2 / Jetson Orin NX or Orin Nano / Ubuntu 22.04）如何部署、配置、验证**

---

## 2. 基础约束与设计依据

### 2.1 已确认的硬件与系统条件

基于 `docs/matirays` 下已完整阅读的两份文档，可确定以下事实：

- `11F1E2` 为 Jetson Orin NX / Orin Nano 工控机平台，提供 **4 个 USB 3.0 Type-A** 接口，适合直连 USB 相机。
- `11F1E2` 预装系统为**纯净系统**，默认**不含 JetPack**，需要后续手工安装 `nvidia-jetpack`。
- `OAK-D-Pro-W` 接口为 **USB 2.0/3.0**。
- `OAK-D-Pro-W` 支持 **Ubuntu / Jetson / C++ / OpenCV / ROS**。
- `OAK-D-Pro-W` 的 RGB 相机为 **12MP (4032x3040)**，双目为 **1MP (1280x800)**。
- `OAK-D-Pro-W` 支持 **H.264 / H.265 / MJPEG 编码**，能力覆盖 **4K/30fps**、**1080p/60fps**。

### 2.2 当前仓库的现状

当前项目已具备一套可复用的视频 UI 逻辑，但视频源仅支持 HTTP MJPEG：

- 视频模块：`videoclient.h/.cpp`
- 首页绑定：`home.h/.cpp`
- 显示控件：`mainwindow.ui` 中的 `videoDisplay`
- 已有能力：
  - 帧显示
  - 录像
  - 截图
  - 断线重连

当前问题不是“UI 没有视频能力”，而是“视频源后端过于单一”。

### 2.3 设计要求

该方案必须满足：

- **高效**：避免把所有视频都压到无线网络上，减少与现有 HTTP/WebSocket/状态轮询链路争抢带宽
- **稳定**：相机故障、掉线、USB 波动不应拖垮主程序
- **快速落地**：第一版能尽快上线，第二版逐步演进
- **可维护**：不要把 OAK、HTTP MJPEG、录像、显示逻辑混在一个类里
- **可读性**：模块边界清晰，配置外置

---

## 3. 总体架构

推荐采用“工控机相机服务 + 上位机统一视频源接口”的分布式架构：

```text
OAK Camera
  -> USB
  -> IPC Camera Service (DepthAI / OpenCV / HTTP API)
       -> /camera/stream.mjpeg
       -> /camera/photo
       -> /camera/record/start
       -> /camera/record/stop
       -> /camera/status
  -> Wireless / LAN
  -> Tenco Home
       -> AbstractVideoSource
            -> MjpegVideoSource
       -> CameraControlClient
```

### 3.1 模块职责

- `Home`
  - 负责 UI 绑定
  - 负责按钮点击
  - 负责日志展示
  - 不负责具体视频采集协议

- `AbstractVideoSource`
  - 定义统一的视频源接口
  - 对 `Home` 暴露一致的控制方式

- `MjpegVideoSource`
  - 封装现有 `VideoClient`
  - 负责接工控机相机服务输出的 MJPEG 预览流

- `CameraControlClient`
  - 调用工控机相机服务
  - 负责拍照、开始录像、停止录像、状态查询

- `OakCameraVideoSource`
  - 保留为开发调试/单机部署兜底能力
  - 不作为当前分布式生产架构的默认主路径

### 3.2 为什么不能直接把 OAK 逻辑塞进 `VideoClient`

不建议在现有 `VideoClient` 里硬塞 OAK 逻辑，原因是：

- 当前 `VideoClient` 明确是 HTTP MJPEG 语义
- OAK 接入是设备采集语义，不是 HTTP 拉流
- 二者的错误模型、重连方式、录像方式都不同
- 混在一起后，后续难以维护与测试

---

## 4. 版本规划

## 4.1 V1 稳定落地版

### 目标

- 首页显示工控机侧 OAK 实时画面
- 上位机可远程触发拍照
- 上位机可远程触发开始/停止录像
- 尽量少改 UI
- 尽量少动现有 HTTP/WebSocket/状态轮询链路

### 技术选型

- 工控机侧设备接入：`DepthAI C++`
- 工控机侧预览/录像：`OpenCV`
- 上位机预览：Qt `QLabel/QImage/QPixmap` + 现有 HTTP MJPEG
- 上位机控制：HTTP JSON 接口

### V1 的原则

- 不优先做 OpenGL
- 不优先做高码率远程视频无线发布
- 不优先做 H.265 设备端录像
- 优先做“工控机本地稳定采集 + 上位机稳定远程控制”

## 4.2 V2 高性能增强版

### 目标

- 提升 CPU/内存效率
- 利用 OAK 设备端编码
- 降低主机录像开销
- 为未来无线远程视频分发留出能力
- 可选引入 OpenGL 优化显示

### 技术选型

- 设备接入：`DepthAI C++`
- 设备端录像：`VideoEncoder (H264/H265/MJPEG)`
- 显示优化：`QOpenGLWidget` 或 `QOpenGLTexture`
- 可选扩展：本地 RTSP / WebRTC / MJPEG 发布服务

---

## 5. V1 完整实现方案

## 5.1 工控机侧实施

### 5.1.1 操作系统准备

目标环境：`Ubuntu 22.04`

先确认系统版本：

```bash
lsb_release -a
uname -a
```

### 5.1.2 安装 JetPack

11F1E2 文档明确写明预装系统默认不带 JetPack，因此必须先补齐：

```bash
sudo apt update
sudo apt install -y nvidia-jetpack
```

安装后建议确认：

```bash
dpkg -l | grep nvidia-jetpack
dpkg -l | grep jetson
```

### 5.1.3 安装基础开发依赖

```bash
sudo apt update
sudo apt install -y \
  build-essential \
  cmake \
  ninja-build \
  pkg-config \
  git \
  libopencv-dev \
  v4l-utils \
  ffmpeg \
  libavcodec-dev \
  libavformat-dev \
  libavutil-dev \
  libswscale-dev
```

如果工控机还需要本地编译 Qt 程序，可继续：

```bash
sudo apt install -y \
  qt6-base-dev \
  qt6-base-dev-tools \
  qt6-tools-dev-tools \
  qt6-declarative-dev
```

### 5.1.4 安装 DepthAI

优先安装与 OAK 文档要求一致的版本。若现场未给定固定版本，可先验证仓库内支持的稳定版。

建议方式：

```bash
git clone https://github.com/luxonis/depthai-core.git
cd depthai-core
git submodule update --init --recursive
cmake -S . -B build -G Ninja -DDEPTHAI_BUILD_EXAMPLES=OFF
cmake --build build --parallel
sudo cmake --install build
```

安装完成后验证：

```bash
pkg-config --modversion opencv4
ls /usr/local/lib | grep depthai
```

### 5.1.5 接入相机并确认枚举

将 OAK-D-Pro-W 插入工控机 USB 3.0 口后执行：

```bash
lsusb
v4l2-ctl --list-devices
ls /dev/video*
```

说明：

- 如果设备能以普通 UVC 模式枚举成 `/dev/video*`，说明可以做兜底采集
- 但生产方案仍建议优先走 `DepthAI`

### 5.1.6 做最小设备验证

先用官方或自写最小 C++ 程序确认 OAK 可连接：

```bash
cd /tmp
cat > oak_probe.cpp <<'EOF'
#include <depthai/depthai.hpp>
#include <iostream>
int main() {
    try {
        dai::Device device;
        std::cout << "OAK connected: " << device.getMxId() << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }
}
EOF

g++ oak_probe.cpp -o oak_probe -std=c++17 $(pkg-config --cflags --libs opencv4) -ldepthai-core
./oak_probe
```

如果此步骤失败，先不要继续改主项目。

### 5.1.7 工控机运行时目录规划

建议约定统一目录：

```text
/opt/tenco/
  bin/
  config/
  logs/
  media/
    snapshots/
    recordings/
```

命令：

```bash
sudo mkdir -p /opt/tenco/{bin,config,logs,media/snapshots,media/recordings}
sudo chown -R $USER:$USER /opt/tenco
```

### 5.1.8 本地运行的 systemd 服务建议

若未来需要工控机本地运行视频代理或诊断程序，建议统一用 `systemd` 管理，而不是手工后台启动。

示例：

```bash
sudo tee /etc/systemd/system/tenco-oak-probe.service >/dev/null <<'EOF'
[Unit]
Description=Tenco OAK Probe
After=network-online.target

[Service]
User=nvidia
WorkingDirectory=/opt/tenco
ExecStart=/opt/tenco/bin/oak_probe
Restart=on-failure
RestartSec=3
StandardOutput=append:/opt/tenco/logs/oak_probe.log
StandardError=append:/opt/tenco/logs/oak_probe.err

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable tenco-oak-probe.service
```

### 5.1.9 5.1 完成后的下一步

完成 5.1 后，说明工控机已经具备：

- OAK 相机可枚举
- DepthAI / OpenCV 开发环境可用
- 本地目录规划已建立
- `systemd` 托管方式已验证

这时**不要直接去运行上位机 UI**，而是继续在工控机侧完成一个最小可用的 `IPC Camera Service`，作为 5.2 项目侧远程预览与远程控制的对接对象。

工控机侧下一步必须继续完成：

1. 实现并启动相机服务进程
2. 提供这些 HTTP 接口：
   - `GET /camera/stream.mjpeg`
   - `GET /camera/status`
   - `POST /camera/photo`
   - `POST /camera/record/start`
   - `POST /camera/record/stop`
3. 照片与录像默认保存到：
   - `/opt/tenco/media/snapshots`
   - `/opt/tenco/media/recordings`
4. 接口返回统一 JSON，至少包含：
   - `success`
   - `message`
   - `path`
   - `cameraConnected`
   - `recording`
5. 用 `curl` / `ffplay` 在工控机本机先验证接口，再和上位机联调

推荐自测命令：

```bash
curl http://127.0.0.1:18080/camera/status
curl -X POST http://127.0.0.1:18080/camera/photo
curl -X POST http://127.0.0.1:18080/camera/record/start
curl -X POST http://127.0.0.1:18080/camera/record/stop
ffplay http://127.0.0.1:18080/camera/stream.mjpeg
```

---

## 5.2 项目侧实施

## 5.2.1 新增统一视频源接口

新增文件：

- `abstractvideosource.h`
- `abstractvideosource.cpp`

接口建议：

```cpp
class AbstractVideoSource : public QObject {
    Q_OBJECT
public:
    enum class State {
        Stopped,
        Connecting,
        Streaming,
        Error
    };

    virtual ~AbstractVideoSource() = default;

    virtual void start() = 0;
    virtual void stop() = 0;
    virtual bool isActive() const = 0;

    virtual bool startRecording(const QString& directory, QString* outPath = nullptr) = 0;
    virtual QString stopRecording() = 0;
    virtual bool isRecording() const = 0;

    virtual bool saveSnapshot(const QString& directory, QString* outPath = nullptr) const = 0;
    virtual QImage lastFrame() const = 0;

signals:
    void frameReceived(const QImage& frame);
    void stateChanged(State state, const QString& message);
};
```

## 5.2.2 兼容现有 MJPEG 后端

新增：

- `mjpegvideosource.h`
- `mjpegvideosource.cpp`

作用：

- 内部组合现有 `VideoClient`
- 对外实现 `AbstractVideoSource`

这样可以保证：

- 旧的 HTTP MJPEG 方案继续可用
- 首页代码以后只依赖统一接口

## 5.2.3 新增远程相机控制客户端

新增：

- `cameracontrolclient.h`
- `cameracontrolclient.cpp`

职责：

- 调用工控机相机服务的 HTTP 接口
- 管理拍照、开始录像、停止录像命令
- 管理状态查询
- 管理请求超时与错误上报

### V1 接口建议

- `GET /camera/stream.mjpeg`
- `GET /camera/status`
- `POST /camera/photo`
- `POST /camera/record/start`
- `POST /camera/record/stop`

建议接口返回：

```json
{
  "success": true,
  "message": "photo saved",
  "path": "/opt/tenco/media/snapshots/oak_photo_20260423_153000.jpg",
  "recording": false,
  "cameraConnected": true
}
```

## 5.2.4 录像实现建议

V1 采用**工控机本地录像**：

- 工控机使用 `cv::VideoWriter`
- 输出为 `.avi` 或 `.mp4`
- 上位机只发开始/停止命令，不承载主录像数据写盘

建议参数：

- 预览流录像：`1280x720 @ 30fps`
- codec 优先：`MJPG`

理由：

- 最稳
- 最容易跨 Ubuntu 22.04 落地
- 对无线链路更友好

## 5.2.5 拍照实现建议

拍照不使用上位机预览截图，而是由工控机尽量使用 still 通道。

优点：

- 分辨率更高
- 不受预览缩放影响
- 更符合 OAK 设备能力

保存格式：

- `.jpg`

## 5.2.6 `Home` 的改造方式

修改文件：

- `home.h`
- `home.cpp`

当前：

- `VideoClient *m_videoClient`

调整为：

- `AbstractVideoSource *m_videoSource`

尽量保持以下函数逻辑不变：

- `initializeVideoDisplay()`
- `handleVideoFrameReceived()`
- `record()`
- `photo()`

但职责改为：

- 预览继续走 `AbstractVideoSource` / `MjpegVideoSource`
- 拍照/录像控制由 `CameraControlClient` 调工控机接口

## 5.2.7 配置系统扩展

修改：

- `configmanager.h`
- `configmanager.cpp`
- `config.json`
- `docs/03_Configuration.md`

建议新增配置：

```json
"video": {
  "backend": "mjpeg_http",
  "streamUrl": "http://192.168.31.7:18080/camera/stream.mjpeg",
  "controlBaseUrl": "http://192.168.31.7:18080",
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

说明：

- `backend`: 当前生产推荐 `mjpeg_http`
- `controlBaseUrl`: 工控机相机控制服务地址
- `deviceId`: 多 OAK 设备时指定 MxId
- `recordMode`: 为 V2 留扩展点

## 5.2.8 CMake 改造

修改：

- `CMakeLists.txt`

新增/保留依赖：

```cmake
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network WebSockets)
```

新增源文件：

- `abstractvideosource.*`
- `mjpegvideosource.*`
- `cameracontrolclient.*`

`oakcameravideosource.*` 和 `DepthAI/OpenCV` 依赖仅在需要单机直连调试时通过 `TENCO_ENABLE_OAK_CAMERA=ON` 启用。
- 调试分支可保留 `oakcameravideosource.*`

---

## 5.3 V1 稳定性要求

### 5.3.1 不与无线链路争带宽

当前项目与工控机之间已经存在多条通信链路：

- 状态轮询 HTTP
- WebSocket 底盘控制
- 其它 HTTP 接口

因此：

- **USB 相机的高质量采集、拍照、录像应优先在工控机本机内存中处理**
- 上位机只拿预览流和控制结果，不承担主录像数据链路
- 远程预览默认控制在可接受码率，不与现有状态/控制链路抢占带宽

### 5.3.2 避免相机异常拖垮主程序

要求：

- 工控机相机服务不可用时只影响视频区，不影响主窗口其他功能
- 相机拔插、采集失败时进入 `Error` 状态，并允许重试
- 视频线程不可阻塞 UI 主线程

### 5.3.3 日志要求

建议日志分类：

- `tenco.video.oak`
- `tenco.video.mjpeg`
- `tenco.video.record`

日志内容至少包括：

- 设备枚举结果
- pipeline 启动/停止
- 录像开始/结束路径
- 相机异常与重试次数

### 5.3.4 保存策略

建议目录：

- 工控机本地：
  - `/opt/tenco/media/snapshots`
  - `/opt/tenco/media/recordings`

桌面开发机环境下：

- 使用当前 UI 已有的保存目录选择能力

---

## 6. V2 高性能增强版

## 6.1 目标

V2 不再以“先跑通”为主，而是进一步优化：

- 降低主机 CPU 占用
- 提升录像质量和效率
- 为未来远程无线查看画面做准备
- 提升显示性能

## 6.2 录像升级为设备端编码

将 `recordMode` 从：

- `host_opencv`

升级为：

- `oak_encoder_h264`
- `oak_encoder_h265`
- `oak_encoder_mjpeg`

### 做法

- 使用 OAK `VideoEncoder`
- 编码发生在设备端
- 主机只接收编码后的 bitstream

优点：

- 减少主机 CPU 压力
- 更适合长时间录像
- 更适合高分辨率或高帧率

### 文件输出建议

- H264 -> `.h264`
- H265 -> `.h265`
- MJPEG -> `.mjpeg`

后处理可选转封装：

```bash
ffmpeg -framerate 30 -i input.h264 -c copy output.mp4
```

## 6.3 显示升级为 OpenGL

仅在以下情况考虑升级：

- 720p/1080p 预览下 `QLabel + QPixmap` CPU 占用明显过高
- 需要做缩放、叠加、标注
- 需要更高刷新率与更平滑渲染

改造方向：

- 新增 `VideoRenderWidget : public QOpenGLWidget`
- 帧到达后上传纹理
- 在 OpenGL 中做缩放与绘制

注意：

- OpenGL 是**显示优化**，不是相机接入方案
- 不应在 V1 未稳定前优先引入

## 6.4 可选远程视频分发

考虑当前项目大量控制链路走无线通信，因此**不要默认把预览高码率视频走无线链路**。

若确有远程查看需求，建议做成**可选能力**：

- 默认：工控机本地高质量处理，上位机按需获取预览
- 远程模式：按需启动轻量视频服务

可选路线：

- 本地 MJPEG 发布服务
- RTSP 服务
- WebRTC

推荐顺序：

1. 工控机本地高质量采集与处理
2. 工控机本地设备端编码录像
3. 如确有需要，再做低码率远程子码流

## 6.5 双码流建议

若未来要兼顾本地高质量与远程无线查看：

- 主码流：
  - 工控机本地显示 / 本地录像
  - 720p / 1080p
- 子码流：
  - 远程无线查看
  - 640x360 或 854x480
  - H264 低码率

这样能显著减少对无线状态链路的干扰。

---

## 7. 推荐代码改造顺序

## 7.1 第一步：结构清理

1. 新增 `AbstractVideoSource`
2. 把现有 `VideoClient` 适配成 `MjpegVideoSource`
3. `Home` 改为只依赖抽象接口

## 7.2 第二步：完成工控机侧相机服务

1. 在工控机上接入 OAK
2. 跑通 `GET /camera/status`
3. 跑通 `GET /camera/stream.mjpeg`
4. 跑通 `POST /camera/photo`
5. 跑通 `POST /camera/record/start` / `POST /camera/record/stop`

## 7.3 第三步：补齐配置和文档

1. `config.json` 增加 `streamUrl` / `controlBaseUrl`
2. `docs/03_Configuration.md` 更新
3. `docs/05_Modules.md` 更新
4. `docs/10_Troubleshooting.md` 增加相机故障说明

## 7.4 第四步：项目侧联调

1. 上位机接入远程 MJPEG 预览
2. 上位机接入远程拍照接口
3. 上位机接入远程录像控制接口
4. 联调日志、错误提示、状态恢复

## 7.5 第五步：V2 优化

1. 设备端编码
2. OpenGL 显示优化
3. 可选远程视频服务

---

## 8. 测试与验收

## 8.1 工控机侧验收

### 设备识别

```bash
lsusb
v4l2-ctl --list-devices
```

### OAK 连接

```bash
/opt/tenco/bin/oak_probe
```

### 相机服务接口验证

```bash
curl http://127.0.0.1:18080/camera/status
curl -X POST http://127.0.0.1:18080/camera/photo
curl -X POST http://127.0.0.1:18080/camera/record/start
curl -X POST http://127.0.0.1:18080/camera/record/stop
```

### 预览流验证

```bash
ffplay http://127.0.0.1:18080/camera/stream.mjpeg
```

### 录像文件验证

```bash
ls -lh /opt/tenco/media/recordings
ffprobe /opt/tenco/media/recordings/xxx.avi
```

### 拍照文件验证

```bash
ls -lh /opt/tenco/media/snapshots
file /opt/tenco/media/snapshots/xxx.jpg
```

## 8.2 项目侧验收

- 首页打开后视频区能在 3 秒内看到工控机输出的预览
- 相机断开时页面不崩溃，仅提示错误
- 工控机相机服务恢复后，预览与控制可恢复
- 点击拍照后，上位机能收到成功反馈，且工控机本地生成可打开的 `.jpg`
- 点击录像开始/停止后，上位机能收到成功反馈，且工控机本地生成可播放的视频文件
- 状态轮询、底盘控制、地图页面不受视频模块阻塞影响

## 8.3 无线链路稳定性验收

验收重点：

- 开启视频后，状态轮询不能出现持续性超时飙升
- WebSocket 控制不能出现明显按键延迟
- 若工控机与上位机通过无线连接，视频方案不得默认挤占业务控制链路

建议测试：

```bash
ping <robot-ip>
```

同时观察：

- 状态日志频率
- WebSocket 控制响应
- CPU 利用率
- 内存占用

---

## 9. 风险与规避

## 9.1 风险：DepthAI 版本与系统不匹配

规避：

- 固定版本
- 安装后先跑最小 probe
- 不在主程序里直接做首次排错

## 9.2 风险：录像编码在主机侧占用高

规避：

- V1 使用 720p30 + MJPG
- CPU 不足时优先切到 V2 设备端编码

## 9.3 风险：无线链路被视频抢占

规避：

- 默认由工控机本地完成高质量采集、拍照、录像
- 上位机仅接预览流与控制接口
- 远程查看做成显式可选模式

## 9.4 风险：相机故障导致 UI 卡死

规避：

- 采集线程与 UI 线程分离
- 所有设备异常只回报状态，不抛到 UI 主循环

---

## 10. 推荐最终落地策略

若按生产标准执行，推荐最终路线是：

### 阶段 A

- 完成 V1
- 工控机本地 USB 直连 OAK
- 上位机远程预览 + 远程拍照 + 工控机本地录像
- 先把系统跑稳

### 阶段 B

- 切到 V2 设备端编码
- 降低主机负载
- 评估是否需要 OpenGL

### 阶段 C

- 若确有远程视频需求，再补低码率远程视频链路
- 但绝不让其默认压到现有控制通信链路上

---

## 11. 本方案的最终判断

在你当前项目、11F1E2 工控机、OAK-D-Pro-W 相机、Ubuntu 22.04、无线控制链路并存的约束下，最佳方案是：

- **V1：工控机侧 DepthAI/OpenCV 相机服务 + 上位机侧 Qt 现有 MJPEG 显示链路 + 远程控制**
- **V2：DepthAI 设备端编码 + 工控机侧服务优化 + 可选 OpenGL 显示优化 + 可选远程子码流**

这条路线兼顾：

- 快速落地
- 维护性
- 稳定性
- 性能
- 后续扩展性
