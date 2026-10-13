# OAK相机采集恢复开发记录

更新日期：2026-10-13
会话 ID：`01a13477-9b6f-7f43-8a41-5e5b77886c15`
任务 ID：FIX-20261013-01
状态：开发、隔离验证、在线部署及真实视频接收验证完成；真实USB故障恢复与长期验收保留

开始时间：北京时间 2026-10-13 19:07。分支与原始服务哈希见[开发方案](OAK相机采集恢复开发方案.md)。开发开始时原服务已恢复，开发与隔离测试未操作在线相机；用户随后明确授权部署，当前在线版本及验证结果见下述部署小节。

范围：纳管独立服务源码，采集失败重建/缓存新鲜度/诊断、HTTP 资源回收及有界测试。P-009 物理掉线诱因和真实设备恢复仍需验收。实施、测试、提交/PR 与具体部署产物按里程碑补记；没有完成的验证不预填成功。

## 实现与行为变化

Tenco `services/oak-camera/` 作为服务版本源，远端隔离目录 `/home/nvidia/tenco-oak-probe-recovery-v1`；原非 Git 工程和在线服务保留。原始源码来自诊断哈希对应版本，第三方 SDK/原安装脚本/PDF/媒体不纳管。本任务没有修改 Qt 产品源码或 src 控制/ROS/Lua。

之前：采集异常后线程退出，缓存未清、主 HTTP 健康假象、无自动恢复；现在：采集源由同一线程创建/释放，捕获异常或默认 5 秒无新帧后清空帧和当前录像，用默认 1～10 秒退避重建，首个合法新帧才置健康，持续健康后复位退避。恢复不自动续录。错误在 SDK 清理前写带时间日志，lastCaptureError 恢复后仍保留，状态追加 frameId/reconnectCount/lastFrameAgeMs。单调时钟超过默认 1 秒的缓存不供流、照片或新录像使用；无健康帧的流返回 503。

HTTP 去除 detached 线程，最多 16 客户、2 秒 socket 读写等待；退出中断 socket、停止相机、join 所有线程再析构状态。绑定端口先于相机启动，重复实例不争用相机，去除 SO_REUSEPORT；新增 CLI 新鲜度/卡帧/重连参数并校验整数尾随字符、NaN/范围/关系。生产 DepthAI 源与假源使用同一恢复循环和 HTTP 实现，fixture 只用于测试。

新增 README/CMake/DepthAI 适配/测试和部署脚本；部署脚本要求期望哈希、检查录像状态、备份/原子替换/重启相机、就绪失败回退。开发阶段完成 bash -n 校验；本次已真实备份、安装和重启，故意触发安装失败/回退演练仍未执行。SDK 打开与销毁可能有内部等待，未承诺与假源相同的停止时延；真实 USB 掉线与 SDK 卡死恢复仍需验收。

## 验证证据

环境：Jetson Ubuntu 22.04.5 ARM64，GCC 11.4.0、DepthAI 2.31.1、OpenCV 4.8.0、Python 3.10.12；本机固定 Qt 6.5.3/MinGW 11.2.0。

| 验证 | 结果与范围 |
|---|---|
| Jetson Debug 完整 configure/build + CTest | 最终 2/2，通过异常/缺设备、卡帧、新鲜度、清缓存、录制停止、退避中断、同对象停止重开及无效参数 |
| Jetson Release 完整 configure/build + CTest | 最终 2/2，通过同一生产恢复/HTTP 的多帧、断流EOF、故障503、恢复、照片/录像、未完成客户请求和退出；无真实USB打开 |
| 源码编译诊断 | 初次仅有继承源码中 trim 未使用警告，已删除该无用函数；最终完整编译通过，测试未失败 |
| 固定 Qt Kit 完整 configure/build + CTest | 16/16，含地图/客户端/视频worker；完整 Qt 构建成功，未更改 UI |
| 真实 Qt VideoClient/worker → Jetson fixture | 18081 隔离端口，初始503后恢复收帧、注入 X_LINK_ERROR、EOF重连后恢复，输出 PASS frames=6 initial_errors=3、退出码0；不操作18080在线服务 |
| 部署脚本 | bash -n 通过；用户授权后实际备份、替换与启动成功，经独立核验；失败回退演练未执行 |
| 开发阶段在线版本保护（历史） | 开发与隔离测试时 PID 12545/NRestarts=0，二进制为原 `385f7930...eeadff`；测试fixture已TERM退出 |
| 当前在线进程/文件一致性 | PID 16018、active/running、NRestarts=0，安装文件及 `/proc/16018/exe` 均与 Release 候选完整哈希一致 |
| 实际网络 MJPEG 连续采样 | 同一连接 45.020 秒、677 帧/677 个不同 JPEG、15.038 FPS，无EOF；最大帧间隔80.7 ms，10次状态均健康，帧序号5473→6149，帧年龄14～54 ms、重连次数0 |
| 真实 Qt 客户端/worker → 在线真实相机 | 固定Kit临时探针 configure/build后读取18080，19.990秒299个合法960×540帧，errors=0、最大间隔86 ms，退出码0；无模拟源/USB操作 |

完整命令见服务 README/统一 AGENTS；本地证据 `tmp/video-diagnosis-20261013/recovery-{ctest-initial,release-verify,final-build-verify}.txt` 和 `qt-recovery-integration.txt`。Qt 固定目录的 CTest `Testing/Temporary/LastTest.log` 保留实际运行。Qt 视频联调探针临时 CMake/源码只作本地证据，服务的可重复测试随源码纳管。

## 具体部署候选与回退

当前保留规则后续变更（2026-10-13，同会话）：用户随后要求始终仅留最新版，CLEAN-20261013-01已删除下述历史旧程序备份和旧工程。下述20:06备份/回退叙述保留部署时事实，当前不能使用已删文件；最新版/清理清单和恢复边界见 [清理记录](相机旧版本与备份清理开发记录.md)，未来切换只临时保留旧程序并在验证后删除。

### 2026-10-13 在线部署授权与检查

同一会话用户明确“是的 现在部署”，授权既有候选切换、备份和就绪失败回退。20:04:48（北京时间）SSH 部署前核对：候选完整哈希与下述值一致；远端脚本与本地逐字节一致（统一 LF），SHA256 `8843f79920b10875eff5ec2ec9f5980d8365bd47233d5f7999ce346a5672dc0d`；ExecStart 仍为既定路径/参数。旧进程 PID 12545/NRestarts=0，HTTP 主进程 active/running，但再次出现 preview/X_LINK_ERROR、cameraConnected=false、recording=false。此现象是本次实时结果，早前已恢复的历史结果保留。只读证据 `tmp/video-diagnosis-20261013/deploy-preflight.txt`；实际执行和验收结果随后补记。

已部署 Release 构建产物：`/home/nvidia/tenco-oak-probe-recovery-v1/build-release/oak_camera_service`，SHA256 `89cb76d052e8c28ed5a8af2648b7f31604f885a7947d26930837b307f389ecd6`。用户授权后切换相机服务，短暂中断预览；实际执行命令：

```bash
sudo bash /home/nvidia/tenco-oak-probe-recovery-v1/scripts/deploy_verified_service.sh /home/nvidia/tenco-oak-probe-recovery-v1/build-release/oak_camera_service 89cb76d052e8c28ed5a8af2648b7f31604f885a7947d26930837b307f389ecd6
```

### 实际切换、备份与核验

20:06:22 脚本创建备份 `/opt/tenco/bin/oak_camera_service.backup-20261013-200622-15962`，SHA256 `385f79300c4e8b32efd14a096239f604262b9738c4ca47b3c7b473b115eeadff`，与原安装程序一致。20:06:26 systemd 启动新进程 PID 16018，20:06:29 日志报告 capture connected；首个就绪响应 frameId=22、年龄6 ms。安装文件和正在执行的 `/proc/16018/exe` 均为上述候选哈希，服务 active/running、NRestarts=0。已核验持续视频与真实Qt接收；未修改unit/启动参数、媒体、Lua或车辆控制，没有再次部署或主动USB故障注入。

首次 SSH 输出备份、健康响应和 Installed，但未回传有效远端退出状态（Paramiko recv_exit_status=-1，本地包装命令非零），不记录为“脚本退出码0”。随后独立 SSH 查询退出码0，核验安装/运行哈希、备份、systemd状态/日志和持续帧，确认实际切换成功。原unit保留的 `ExecStartPre=-pkill -9 -f ...` 会匹配宽泛命令行，部署时新增stderr提示针对部署脚本PID 15962的pkill权限失败；SSH退出状态缺失与其具体关联尚未专项验证，登记P-010待收窄清理范围。本次未因退出状态缺失重复替换服务。

证据位于本地忽略目录：`deploy-{preflight,execution,online-state,logs-process}.txt`、`deploy-continuous-stream.json`、`deploy-qt-real-stream.txt`，临时可重复核验程序为 `verify-production-stream.py` 和 `qt-production-probe/`，不保存图像或凭据。真实Qt临时CMake配置提示未使用C编译器变量/缺少可选Vulkan头，C++探针完整编译和运行通过，未改产品构建。

20:18:07 再查仍为PID16018/NRestarts=0、健康且未录像，frameId=10473、帧年龄12ms、reconnectCount=0/lastCaptureError为空；距相机连接约11分38秒，短时上线检查期间没有新采集故障。证据 `deploy-final-state.txt`。此检查不是8～24小时连续负载验收。

脚本就绪失败回退分支仍未真实触发；如需人工回退，先确认没有录像，使用上述备份原子恢复安装程序并重启 `tenco-oak-camera.service`，核验旧程序哈希和真实连续帧。主动拔插/设备消失测试须确认现场条件；当前结果证明新版上线后正常采集/传输，不能证明 USB 供电/线材缺陷已解决。

## 未完成、记录与交付

P-009：实现、隔离验证、在线部署、旧程序备份和真实持续视频接收已完成；失败回退演练、真实 USB/设备掉线重连、SDK卡死及掉线具体诱因未验收。P-004 8～24 小时混合负载、P-010旧unit宽泛pkill清理保留。当前停在已部署/持续视频验证完成；继续先核对在线哈希/状态/最近采集错误，按明确现场条件安排真实故障恢复与长期验收，不重复要求本次部署授权。

两仓方案/记录、统一 AGENTS/总览/状态/变更/问题/会话与镜像随交付更新；提交/推送及草稿 PR 以真实结果补记。功能基线需区分 Qt/ROS 原有行为和新增独立服务源码。新自动 CI 不冒充 Jetson 实测或真实设备验收。

## 已核验的交付结果

实现提交 Tenco `3abc02ce16528976a3d6dc17a893d808254655b8`（新增服务功能基线）和 src `5e3a713e69f885f53284da2eef61e82b2ec21c50`（仅文档）已推送。[Tenco #20](https://github.com/DAC-Code25/Tenco/pull/20) 与 [src #6](https://github.com/DAC-Code25/sensor-fusion-ros2/pull/6) 已核验 OPEN Draft、base 为诊断分支、head 为对应任务分支。Tenco 使用 gh 创建/查询时发生 GitHub 443 连接超时；已通过 REST 先查同 head 无重复PR，再创建并核验成功，保留此次网络失败，不将成功说成从未失败。没有合并 PR。

完整空间/两仓独立镜像、字段/本地链接、源码指纹及 diff --check 通过，源码/记录未包含登录凭据。目标机11份服务文件与本地逐个SHA256一致，`verified-candidate.json` 记录源码提交与候选哈希，两份中文开发文档已复制到隔离项目docs。随后补交交付元数据，最终文档提交从PR HEAD核对，不因自引用再修改源码版本清单。

旧诊断 src #5/Tenco #19 的最终HEAD检查19:25全部成功，D-003关闭。本任务补记部署后新自动CI D-004按最终PR HEAD待核验，不预填成功；现有云端工作流覆盖Qt/ROS，独立服务测试和在线采集证据来自本次目标机实际运行。部署补记沿用同一任务分支/草稿PR，不改变源码提交或构建产物；上述现场遗留继续保留。

部署补记 Tenco `9a948c8d302e2c86c1a7e87fec7a9ce465d88d6e` / src `83f37bc24280b5615a96ca577c88d6a36c46744b` 已推送，PR #20/#6标题/描述已更新并实际核验OPEN Draft、base诊断分支；该HEAD Tenco四项Debug/Release及src humble仍in_progress，D-004保留。完整空间、两仓独立镜像和diff检查通过，两仓与上游0/0、工作树干净。随后补交交付元数据，最终文档SHA从PR HEAD核对，不自引用重复提交。目标机隔离docs随此记录更新并保存实际上线清单；线上程序未因文档补记再次替换/重启。
