# 工控机 OAK 相机 HTTP 服务

更新日期：2026-10-13
会话 ID：`01a13477-9b6f-7f43-8a41-5e5b77886c15`

独立 C++17/DepthAI/OpenCV 服务，源自目标机 `/home/nvidia/tenco-oak-probe` 的项目代码。本目录不链接到 Windows Qt 应用；厂商 SDK 从目标机 `/opt/ros/humble` 获取，不复制进仓库。源码首次纳管与原始哈希见 [开发方案](../../docs/develop/OAK相机采集恢复开发方案.md) 和 [诊断记录](../../docs/develop/相机断流排查开发记录.md)。未纳管原工程的 build、PDF、媒体、安装脚本或第三方代码。

在 Ubuntu 22.04 / Jetson ARM64（已有 DepthAI 2.31.1、OpenCV 4.8）构建：

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DCMAKE_PREFIX_PATH=/opt/ros/humble
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

`oak_capture_recovery` 测试真实恢复循环的设备缺失、读帧异常、卡帧、缓存、录像不自动续录及停止/重开。`oak_http_recovery` 运行同一 HTTP/恢复代码、替换为测试采集源，验证多帧、503/EOF、故障重连、媒体接口及客户连接回收。测试不占用 USB，也不操作原在线服务。实际 USB/设备故障恢复须部署后另验收。

兼容接口：`GET /camera/status`、`GET /camera/stream.mjpeg`、`POST /camera/photo`、`POST /camera/record/start`、`POST /camera/record/stop`。status 的原字段保留；新增 `frameId`（重连不倒退）、`lastFrameAgeMs`（无帧 -1）、`reconnectCount`（累计采集失败）、`lastCaptureError`（恢复后仍保留最近故障）。健康状态要求收到合法新帧，使用单调时钟；未连接或帧过期的流返回 503，不发旧缓存。

默认画面保持 960×540@15、JPEG 70。采集连续 5 秒无合法新帧则重建；帧超过 1 秒即不作为正常图像提供；失败退避 1、2、4、8、10 秒，持续健康 5 秒后复位退避。失败关闭当前录像，恢复后不会自动续录。

原生存档：实际 IMX378 为4056×3040，完整ISP YUV420经USB3传输，预览独立。拍照单次JPEG编码质量90；录像为H.264/yuv420p分片MP4，libx264 superfast、CRF23、4编码线程、3帧共享缓冲队列。按真实帧时间戳播放，队列满淘汰最旧未编码帧并累计统计；不以固定预览FPS加速播放。拍照/开始录像拒绝超过1秒的原生帧；原生或预览连续5秒卡帧都会重建，故障关闭并封装录像，不自动续录。空录像/写入/封装错误返回失败。

`--capture-jpeg-quality` 控制照片质量（1～100），`--recording-crf` 控制录像质量（0～51，越小文件越大）。默认两个保存目录均为当前工作目录下Capture；目标机必须安装本目录的 `systemd/20-native-capture.conf` 到 `/etc/systemd/system/tenco-oak-camera.service.d/20-native-capture.conf`，使开机启动和手动重启使用绝对路径 `/home/nvidia/tenco-oak-probe-recovery-v1/Capture`。drop-in同时清除旧unit宽泛pkill；systemd停止自己的cgroup。既有业务媒体不迁移/删除。

构建依赖新增pkg-config、libavformat/libavcodec/libavutil开发库及含libx264的FFmpeg；测试需要ffmpeg/ffprobe。原生MP4在Orin Nano低照度实测约4～5FPS，预览15FPS是配置目标，不等于原生录像帧率；容量/负载随场景变化，完整尺寸优先。具体实测/上线状态见 [原生采集记录](../../docs/develop/OAK原生拍照录像开发记录.md)。原生硬件JPEG受宽度32倍数限制，不使用4K裁剪或预览放大冒充原生。

status新增sensorName/nativeWidth/nativeHeight、captureJpegQuality/recordingCrf、recordingCodec/recordingContainer、nativeFrameId/lastNativeFrameAgeMs、recordingFrames/recordingDroppedFrames/lastRecordingError；recording仅在编码线程仍活动时为true。编码线程仅持有媒体状态短锁，状态/MJPEG不等待磁盘封装。正常停止会等待有界队列编码并封装，分片减少异常退出对整段文件的影响，不承诺断电零丢失。

新增参数：`--frame-freshness-ms`、`--no-frame-timeout-ms`、`--reconnect-interval-ms`、`--reconnect-max-interval-ms`，其他参数见 `--help`；数值/关系校验失败明确退出。每次采集连接/失败写带时间的日志。最多 16 个客户连接，socket 读写超时 2 秒，退出中断客户并回收线程。

DepthAI Device 的打开/销毁使用 SDK 自身超时，无法保证物理设备/SDK 卡死时与假采集源相同的停止时延；systemd 超时兜底和真实设备开关/掉线须现场验证。HTTP 服务成功不等于 USB 供电、线材或相机硬件缺陷已解决。

部署前先查 `systemctl cat tenco-oak-camera.service`，确认 ExecStart 指向 `/opt/tenco/bin/oak_camera_service`；执行 `scripts/deploy_verified_service.sh` 时传入经过验证的二进制和期望 SHA256，在已授权维护窗口通过sudo运行。脚本拒绝录像中的切换，暂存旧程序、原子替换并重启相机服务；失败先恢复旧程序。连续三次健康且frameId增长、安装哈希一致后，删除本次临时旧程序，成功回退后也删除重复副本，只保留当前有效版本。不会安装依赖、pkill其他程序、改当前systemd/Lua/车辆控制或删除业务媒体。

2026-10-13用户要求清理长期旧版本/备份；原 `/home/nvidia/tenco-oak-probe` 与上线旧程序备份进入清理范围，最新版源码/Release仍在 `/home/nvidia/tenco-oak-probe-recovery-v1`。当前恢复方法以受控源码重新构建/部署为准，不能使用已删除备份。实际清单与验证见 [清理记录](../../docs/develop/相机旧版本与备份清理开发记录.md)。
