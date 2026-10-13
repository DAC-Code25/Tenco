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

新增参数：`--frame-freshness-ms`、`--no-frame-timeout-ms`、`--reconnect-interval-ms`、`--reconnect-max-interval-ms`，其他参数见 `--help`；数值/关系校验失败明确退出。每次采集连接/失败写带时间的日志。最多 16 个客户连接，socket 读写超时 2 秒，退出中断客户并回收线程。

DepthAI Device 的打开/销毁使用 SDK 自身超时，无法保证物理设备/SDK 卡死时与假采集源相同的停止时延；systemd 超时兜底和真实设备开关/掉线须现场验证。HTTP 服务成功不等于 USB 供电、线材或相机硬件缺陷已解决。

部署前先查 `systemctl cat tenco-oak-camera.service`，确认 ExecStart 指向 `/opt/tenco/bin/oak_camera_service`；保留原二进制及校验哈希，再执行 `scripts/deploy_verified_service.sh`。脚本需要明确传入经过验证的二进制、期望 SHA256，并通过 sudo 在已确认维护窗口运行。它会拒绝录像中的切换，备份二进制，原子替换并仅重启相机服务；就绪失败自动恢复备份并重新启动。不会安装依赖、pkill 其他程序、改 systemd/Lua/车辆控制或删除媒体。回退备份路径由脚本输出。
