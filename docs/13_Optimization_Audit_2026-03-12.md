# Tenco 全面审查与优化清单（2026-03-12）

目的：明确“当前已完成的优化”和“仍可继续改进的点”，避免误判为“已无可优化空间”。

结论先行：

- 当前项目已经完成一轮高价值企业化改造（构建、稳定性、测试、发布基础）。
- 但从企业长期维护角度，仍有可持续优化空间，尤其是 **大类拆分、性能隔离、自动化验证深度**。

---

## A. 已完成优化（本次审查确认）

- 构建系统统一为 CMake，已移除 qmake 流程。
- 网络健壮性增强：重连退避、超时保护、状态轮询回退。
- 配置能力增强：配置校验、`--config` 覆盖。
- 协议模块化：状态字段与地址映射抽离为 `statusprotocol.*`。
- 首页状态展示拆分：`Home` 中状态解析/UI 映射下沉为 `home_status_presenter.*`。
- 路径规划升级：由边数优先改为加权最短路（`routepathfinder.*`）。
- 地图持久化增强：原子写入 + schema 版本 + 独立文档编解码层（`mapdocument.*`）。
- 测试覆盖扩展：`RouteFollower`、`ConfigManager`、`RoutePathFinder`、`StatusProtocol`、`MapDocument`。
- 交付链路补齐：CMake presets、CPack、Windows 打包脚本。

---

## B. 仍可继续优化的重点（按优先级）

### P0（高收益，建议优先）

1) `Home` / `Map` 继续拆分职责  
- 现状：`home.cpp` 与 `map.cpp` 仍是超大文件，维护成本高。  
- 风险：改动易引发跨功能回归。  
- 建议：  
  - `Home` 拆成 `HomeTelemetryPresenter`、`HomeControlCoordinator`、`HomeMediaPresenter`。  
  - `Map` 拆成 `MapUiController`、`MapRouteDispatcher`、`MapSceneRenderer`。  

2) GPS 保存协议抽象  
- 现状：`submitOriginCommand()` 仍包含较多字段硬编码。  
- 建议：提取 `GpsConfigSerializer` + 协议常量表，支持 schema 版本和字段校验。  

3) 回归测试层级提升  
- 现状：已有单测，但缺 UI/模块协作级回归。  
- 建议：新增  
  - `Map` 文件加载后场景状态一致性测试（半集成）；  
  - `Home` 状态字段映射测试（mock packet 驱动）。  

### P1（中收益）

4) 视频解码线程隔离  
- 现状：MJPEG 解码与 UI 主线程耦合，弱机型可能卡顿。  
- 建议：解码放 worker，主线程只接收“最新帧”（丢旧帧保实时性）。  

5) 路径代价模型可配置化  
- 现状：`RoutePathFinder` 惩罚参数固定。  
- 建议：把边惩罚/弧线惩罚下沉到 `config.json`，联调时可动态调参。  

6) 安全配置治理  
- 现状：`authToken` 在 `config.json` 明文可选。  
- 建议：支持环境变量覆盖（如 `TENCO_AUTH_TOKEN`）并在日志中脱敏。  

### P2（长期演进）

7) CI 质量门禁增强  
- 增加 clang-tidy/clang-format 检查与失败门禁。  
- 增加多工具链矩阵（MSVC + MinGW）。  

8) 发布自动化增强  
- 打包脚本中自动接入 `windeployqt`（当前文档有建议，但未全自动实现）。  
- 增加版本号自动注入和 Release Notes 模板生成。  

9) 可观测性增强  
- 增加崩溃转储（minidump）和关键链路指标（连接成功率、重连次数、帧率）。  

---

## C. 文档同步状态（本轮已更新）

- 已同步：`docs/00_Enterprise_Process_Mapping.md`
- 已同步：`docs/04_Architecture.md`
- 已同步：`docs/05_Modules.md`
- 已同步：`docs/06_Network_and_Protocol.md`
- 已同步：`docs/07_Dev_Workflow.md`
- 已同步：`docs/08_Testing_and_CI.md`
- 已同步：`docs/11_Roadmap.md`
- 已同步：`docs/12_AI_Project_Learnings.md`
- 已同步：`docs/README.md` 目录索引

---

## D. 建议执行顺序（下一轮）

1. 先做 `Home` 拆分（低风险子模块先拆：状态展示和媒体展示）。  
2. 再做 GPS 协议抽象（保证协议变更可控）。  
3. 最后补 UI/半集成回归测试和 CI 门禁，锁住质量。
