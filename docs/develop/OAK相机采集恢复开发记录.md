# OAK相机采集恢复开发记录

更新日期：2026-10-13
会话 ID：`01a13477-9b6f-7f43-8a41-5e5b77886c15`
任务 ID：FIX-20261013-01
状态：开发与隔离验证完成；在线部署待确认

开始时间：北京时间 2026-10-13 19:07。分支与原始服务哈希见[开发方案](OAK相机采集恢复开发方案.md)。当前原服务已恢复且正在采集；开发使用隔离副本，不操作运行中相机。

范围：纳管独立服务源码，采集失败重建/缓存新鲜度/诊断、HTTP 资源回收及有界测试。P-009 物理掉线诱因和真实设备恢复仍需验收。实施、测试、提交/PR 与具体部署产物按里程碑补记；没有完成的验证不预填成功。

## 实现与行为变化

Tenco `services/oak-camera/` 作为服务版本源，远端隔离目录 `/home/nvidia/tenco-oak-probe-recovery-v1`；原非 Git 工程和在线服务保留。原始源码来自诊断哈希对应版本，第三方 SDK/原安装脚本/PDF/媒体不纳管。本任务没有修改 Qt 产品源码或 src 控制/ROS/Lua。

之前：采集异常后线程退出，缓存未清、主 HTTP 健康假象、无自动恢复；现在：采集源由同一线程创建/释放，捕获异常或默认 5 秒无新帧后清空帧和当前录像，用默认 1～10 秒退避重建，首个合法新帧才置健康，持续健康后复位退避。恢复不自动续录。错误在 SDK 清理前写带时间日志，lastCaptureError 恢复后仍保留，状态追加 frameId/reconnectCount/lastFrameAgeMs。单调时钟超过默认 1 秒的缓存不供流、照片或新录像使用；无健康帧的流返回 503。

HTTP 去除 detached 线程，最多 16 客户、2 秒 socket 读写等待；退出中断 socket、停止相机、join 所有线程再析构状态。绑定端口先于相机启动，重复实例不争用相机，去除 SO_REUSEPORT；新增 CLI 新鲜度/卡帧/重连参数并校验整数尾随字符、NaN/范围/关系。生产 DepthAI 源与假源使用同一恢复循环和 HTTP 实现，fixture 只用于测试。

新增 README/CMake/DepthAI 适配/测试和部署脚本；部署脚本要求期望哈希、检查录像状态、备份/原子替换/仅重启相机、就绪失败回退。仅运行 bash -n 校验，尚未执行真实安装/回退。SDK 打开与销毁可能有内部等待，未承诺与假源相同的停止时延；真实 USB 掉线与 SDK 卡死恢复仍需验收。

## 验证证据

环境：Jetson Ubuntu 22.04.5 ARM64，GCC 11.4.0、DepthAI 2.31.1、OpenCV 4.8.0、Python 3.10.12；本机固定 Qt 6.5.3/MinGW 11.2.0。

| 验证 | 结果与范围 |
|---|---|
| Jetson Debug 完整 configure/build + CTest | 最终 2/2，通过异常/缺设备、卡帧、新鲜度、清缓存、录制停止、退避中断、同对象停止重开及无效参数 |
| Jetson Release 完整 configure/build + CTest | 最终 2/2，通过同一生产恢复/HTTP 的多帧、断流EOF、故障503、恢复、照片/录像、未完成客户请求和退出；无真实USB打开 |
| 源码编译诊断 | 初次仅有继承源码中 trim 未使用警告，已删除该无用函数；最终完整编译通过，测试未失败 |
| 固定 Qt Kit 完整 configure/build + CTest | 16/16，含地图/客户端/视频worker；完整 Qt 构建成功，未更改 UI |
| 真实 Qt VideoClient/worker → Jetson fixture | 18081 隔离端口，初始503后恢复收帧、注入 X_LINK_ERROR、EOF重连后恢复，输出 PASS frames=6 initial_errors=3、退出码0；不操作18080在线服务 |
| 部署脚本 | bash -n 通过；真实部署/备份回退未执行 |
| 在线版本保护 | 服务仍 PID 12545/NRestarts=0，安装二进制哈希仍为原 `385f7930...eeadff`；测试fixture已TERM退出 |

完整命令见服务 README/统一 AGENTS；本地证据 `tmp/video-diagnosis-20261013/recovery-{ctest-initial,release-verify,final-build-verify}.txt` 和 `qt-recovery-integration.txt`。Qt 固定目录的 CTest `Testing/Temporary/LastTest.log` 保留实际运行。Qt 视频联调探针临时 CMake/源码只作本地证据，服务的可重复测试随源码纳管。

## 具体部署候选与回退

Release 候选：`/home/nvidia/tenco-oak-probe-recovery-v1/build-release/oak_camera_service`，SHA256 `89cb76d052e8c28ed5a8af2648b7f31604f885a7947d26930837b307f389ecd6`。切换仅相机服务，会短暂中断预览；原服务现已正常，当前开发请求未明确在线切换，因此待用户确认。已准备命令：

```bash
sudo bash /home/nvidia/tenco-oak-probe-recovery-v1/scripts/deploy_verified_service.sh /home/nvidia/tenco-oak-probe-recovery-v1/build-release/oak_camera_service 89cb76d052e8c28ed5a8af2648b7f31604f885a7947d26930837b307f389ecd6
```

脚本先保留 `/opt/tenco/bin/oak_camera_service.backup-<时间>-<PID>`，就绪失败自动恢复；不改 systemd 单元或重启其他服务。成功后需验收持续帧、状态 frameId/年龄、日志和真实采集恢复；主动拔插/设备消失测试须确认现场条件。不能从模拟源通过推断 USB 供电/线材缺陷已解决。

## 未完成、记录与交付

P-009：实现和隔离验证已完成；在线部署、备份回退演练、真实 USB/设备重连以及掉线具体诱因未验收。P-004 8～24 小时混合负载仍保留。当前工作停止于准备可审核候选，继续先核对候选哈希/在线录像状态和现场切换确认。

两仓方案/记录、统一 AGENTS/总览/状态/变更/问题/会话与镜像随交付更新；提交/推送及草稿 PR 以真实结果补记。功能基线需区分 Qt/ROS 原有行为和新增独立服务源码。新自动 CI 不冒充 Jetson 实测或真实设备验收。

## 已核验的交付结果

实现提交 Tenco `3abc02ce16528976a3d6dc17a893d808254655b8`（新增服务功能基线）和 src `5e3a713e69f885f53284da2eef61e82b2ec21c50`（仅文档）已推送。[Tenco #20](https://github.com/DAC-Code25/Tenco/pull/20) 与 [src #6](https://github.com/DAC-Code25/sensor-fusion-ros2/pull/6) 已核验 OPEN Draft、base 为诊断分支、head 为对应任务分支。Tenco 使用 gh 创建/查询时发生 GitHub 443 连接超时；已通过 REST 先查同 head 无重复PR，再创建并核验成功，保留此次网络失败，不将成功说成从未失败。没有合并 PR。

完整空间/两仓独立镜像、字段/本地链接、源码指纹及 diff --check 通过，源码/记录未包含登录凭据。目标机11份服务文件与本地逐个SHA256一致，`verified-candidate.json` 记录源码提交与候选哈希，两份中文开发文档已复制到隔离项目docs。随后补交交付元数据，最终文档提交从PR HEAD核对，不因自引用再修改源码版本清单。

旧诊断 src #5/Tenco #19 的最终HEAD检查19:25全部成功，D-003关闭。本任务新自动CI D-004待核验，不预填成功；现有云端工作流覆盖Qt/ROS，独立服务测试证据来自本次Jetson实际运行。在线切换仍未执行，P-009保留，最后停在可审核候选与明确部署/回退命令。
