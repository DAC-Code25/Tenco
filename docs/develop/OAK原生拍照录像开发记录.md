# OAK 原生拍照录像开发记录

任务 ID：CAPTURE-20261013-01；开始日期：2026-10-13；会话 ID：`01a13477-9b6f-7f43-8a41-5e5b77886c15`。

方案：[开发方案](OAK原生拍照录像开发方案.md)。分支：Tenco `feat/oak-native-capture-v1`（基线6f4f383），src `docs/oak-native-capture-v1`（基线704fa11，仅文档）。

初始阶段记录（上线前）：预检和能力探测。Capture 的实际路径为 `/home/nvidia/tenco-oak-probe-recovery-v1/Capture`；当前业务输出仍在 `/opt/tenco/media`，不能称为已切换。正在读取真实 CAM_A 原生能力与编码限制，再实现、构建、媒体验证和上线。

21:30～21:48：实际探测 CAM_A IMX378 4056×3040，CAM_B/C OV9282；当前USB3 5000Mbps。探测小程序初始g++缺nop包含路径失败，改用SDK CMake目标后成功。首版独立Release完整构建2/2、固定Qt完整构建16/16通过；真实隔离候选发现video口4K上限，改ISP后又发现MJPEG宽度32倍数限制，均未产出原生媒体、未安装候选，finally恢复旧服务。按方案修订为完整ISP YUV420传输/单次JPEG拍照/直接YUV H.264录像，再构建3项回归与真实媒体验证。这些隔离通过不冒充现场原生功能已完成。

初始待办（实现/部署/验证现已完成，见后文）：实现、原生媒体尺寸/容器/解码验证、部署、文档与PR交付。既有 P-009 USB/SDK现场验收、P-004长期负载、D-004历史控制CI失败继续保留；不扩大为车辆控制修复。

## 上线前验证里程碑

已完成最终源码：完整ISP 4056×3040 YUV420共享缓存、JPEG90拍照、H.264 superfast/CRF23 MP4独立编码线程、有界3帧队列/实际PTS/统计/错误、2秒分片、原生单路卡帧恢复；systemd drop-in统一Capture路径并移除宽泛pkill。当前线上仍是旧安装版，尚未切换。

真实隔离验证：CRF23方案照片1,264,034字节、录像63,179,106字节/15.081688秒/68帧≈4.51FPS，队列丢102帧；原生宽高4056×3040、H.264/yuv420p/High，ffmpeg和Ubuntu现有GStreamer完整解码均退出0。现场低照度，未承诺所有光照/运动负载下同等容量或FPS。CRF18/ultrafast对照117帧/235,978,344字节，不能把文件缩小比例当作同帧率对比。进一步硬件填边探测ImageManip最大宽4056拒绝，0帧，探测脚本恢复旧服务；不使用低分辨率替代。

最终Release完整build/3项CTest：native录制资源/错误、采集恢复、HTTP原生媒体/异常封装/原生单路卡帧/停止。第一次新增卡帧测试错误期待拍照503，实际兼容API沿用500/success=false，测试失败后修正为检查success并等待故障录像停止封装；2026-10-13最终3/3通过，产品HTTP码未改。Qt固定Kit完整configure/build/16项CTest通过。候选SHA256：`f0450ba8177a760df0ec001e143925188443a3d190d9a75f2c8d3cd7bc862390`。

下一步：已授权在线安装候选和drop-in；事务临时保留旧程序/配置，任何就绪/现场媒体失败恢复，持续新帧和解码验证后删除临时备份。之后记录实际版本、清理明确测试媒体/产物、同步文档镜像和两仓草稿PR。P-011原生录像帧率/负载验收新增；USB/SDK故障、长期与旧控制CI遗留继续保留。

## 22:08 实际部署完成

源码功能提交 `af6e73032ac6b15099692e150c22579fd663b696`，二进制完整SHA256同上述候选。22:08:26启动PID24896，事务脚本22:08:57退出0，安装/运行exe/Release哈希一致，active/running、NRestarts=0。effective ExecStartPre已为空，WorkingDirectory为最新源码，两个绝对目录为Capture。连续3次健康且preview/native frameId增长通过；照片2,513,150字节、录像58,639,407字节/12.145688秒/62帧≈5.10FPS，队列丢80帧，4056×3040/H.264/yuv420p；Ubuntu GStreamer与ffmpeg完整解码退出0，录制期间状态持续健康、无采集/录像错误。停止后未自动续录。

临时旧二进制 `/opt/tenco/bin/oak_camera_service.capture-transaction-old` 已在上述验证全部完成后删除；原drop-in不存在，未生成配置备份。旧unit文件未删，由受控drop-in覆盖；回退不再依赖旧文件，需Git源码重建。具体现场明细仅留本地忽略证据 `tmp/video-diagnosis-20261013/native-capture-deployment.json` 与远端同名记录，不提交照片/录像。

当前收尾：真实45秒HTTP流/20秒Qt客户端验证、明确实验产物清理、元数据与中文记录同步、文档镜像和草稿PR；不扩大测试或主动USB注入。P-010宽泛pkill问题已由本次effective配置清除/维护exit0验证解决，其他已有遗留保留。

## 收尾验证与清理

45.015秒真实HTTP MJPEG559帧/559不同JPEG、12.418FPS、最大间隔165.4ms、10次状态健康且frameId增长，无EOF；真实Qt生产VideoClient/worker19.997秒233帧/errors0/最大间隔162ms。实际预览低于15FPS配置目标，完整原生传输有成本，P-011包括此项。原生录像性能/容量不以预览替代。

明确清理15项已记录实验文件/旧试验照片录像/fixture可执行程序和临时探测构建，共302,832,244字节约288.80MiB；保留最新上线验证照片/MP4、既有业务media、SDK/PDF/历史/记录。远端deployed-version/verified-candidate元数据已更新为功能af6e730和当前Release哈希；受控源码与远端文件收尾将核对。相机仍active/开机enabled，未再次重启、未操作车辆/USB注入。

遗留：P-011原生15FPS/明亮和运动场景/混合负载与容量增长尚未验收；P-004/P-009/D-004继续保留。清理旧PR#21/#7最终HEAD检查本次核验全部SUCCESS，D-005关闭；本任务新PR云端检查D-007独立跟踪。当前中文提交/PR交付正进行，不称已合并。

## 交付与下一步

Tenco功能提交af6e730/文档f81fb9a、src文档2848006已实际推送，[Tenco#22](https://github.com/DAC-Code25/Tenco/pull/22)/[src#8](https://github.com/DAC-Code25/sensor-fusion-ros2/pull/8)核验OPEN Draft，base为对应清理分支，未合并。第一交付HEAD的四项Qt CI排队/运行、src humble运行(D-007)，不冒称全绿；最终文档补记SHA从PR HEAD读取。完整工作空间文档/字段链接/镜像/源码指纹/diff检查通过。

检查发现snapshot在首次源码未提交快照后仍可能保留旧code_baseline，当前已按实际功能提交af6e730纠正根元数据，再生成镜像。工具后续晋升规则修复登记D-008，不混入本次相机功能。当前实际源码/二进制/状态以本记录和上线哈希为准。

下一位先读当前状态、P-011/D-007/D-008及在线status/实际exe哈希，核验新PR最终HEAD检查；原生性能和明亮/运动/长期混合负载按单独任务继续，不把本次正常采集当物理USB恢复通过。Capture已有最新验收媒体，其余旧业务媒体保留，不自动清业务照片录像。实现/上线/当前明确验证已完成；长期/USB/性能边界如实保留。
