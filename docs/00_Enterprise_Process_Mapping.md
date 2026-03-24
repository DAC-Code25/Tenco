# 企业标准开发流程 → Tenco 项目落地映射（总纲）

目标：把“企业里一套可交付的软件开发流程”拆成可执行步骤，并映射到本项目的**具体文件/命令/产物/检查项**，让你知道：

- 每一步应该产出什么（文档/代码/测试/CI/版本/包）
- 这些产物在 **Tenco** 里应该放在哪里
- 目前项目“已具备/缺失/建议下一步做什么”

---

## 0. 项目范围与验收（需求阶段）

**企业做法**

- 定义“做什么/不做什么”（Scope）
- 定义验收标准（Acceptance Criteria）：功能点、性能、稳定性、异常处理、兼容性
- 定义目标运行环境：OS、Qt 版本、网络条件、设备接口协议

**在 Tenco 中对应**

- 功能总览与快速开始：`README.md`
- 详细范围与验收建议：`docs/01_Project_Overview.md`

**现状**

- 已有：项目功能概览、构建说明、`config.json` 说明（`README.md`）
- 已有：`docs/02_Build_and_Run.md` 冒烟 checklist（可重复执行）

**建议落地（优先级高）**

- 持续维护冒烟 checklist，并把新增功能的验收步骤同步写入

---

## 1. 架构与技术方案（设计阶段）

**企业做法**

- 模块拆分：UI/业务/协议/存储/设备层
- 线程模型：哪些在 UI 线程，哪些在工作线程
- 核心数据流与状态机：重连策略、视频状态、路线执行状态
- 配置与可部署性：IP/端口/URL 从配置读取，不写死

**在 Tenco 中对应**

- 架构说明：`docs/04_Architecture.md`
- 关键模块指南：`docs/05_Modules.md`
- 配置：`config.json` + `configmanager.h/.cpp`
- 网络拆分：`statusclient.*` / `chassisclient.*` / `videoclient.*` / `homenetworkworker.*`

**现状**

- 已有：网络地址从 `config.json` 读取（`ConfigManager::NetworkConfig`）
- 已有：从“Home 大而全”拆出独立模块（Status/Chassis/Video/RouteFollower）
- 仍可改进：目录仍是扁平结构（企业常见 `src/ include/ tests/ docs/`）

---

## 2. 工程与依赖管理（项目初始化/工程化）

**企业做法**

- 明确构建系统（统一 CMake）与依赖版本（Qt 版本、编译器）
- 工程可重复构建：一台新机器按文档能编译
- 最小化“机器差异”（环境变量、PATH、Kit）

**在 Tenco 中对应**

- 工程文件：`CMakeLists.txt` + `CMakePresets.json`
- Qt 模块依赖：`find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network WebSockets Core5Compat)`
- 资源：`resources.qrc` / `mainwindow.ui` / `app_icon.rc`
- 忽略规则：`.gitignore`

**现状**

- 已统一为 CMake 构建，支持预设、单测开关与告警开关
- 已固定运行产物目录到 `../bin`，便于联调与部署脚本复用

**建议落地（中优先级）**

- 增加多平台/多编译器构建矩阵（MSVC + MinGW）并在 CI 中并行验证

---

## 3. 开发实现（编码阶段）

**企业做法**

- 小步提交、可回滚：每次改动聚焦一个主题
- 约定日志、错误处理、边界检查
- 封装外设协议：避免 UI 代码直接散落协议细节

**在 Tenco 中对应**

- UI 主容器：`mainwindow.h/.cpp` + `mainwindow.ui`
- 首页业务编排：`home.h/.cpp`
- 地图编辑与路线：`map.h/.cpp` + `mapgraphicsview.h/.cpp`
- 网络/协议封装：`statusclient.*` / `chassisclient.*` / `videoclient.*`
- 路线控制算法：`routefollower.*`
- 配置：`configmanager.*`

---

## 4. 自测与联调（开发自测阶段）

**企业做法**

- 开发自测（Dev Test）+ 冒烟用例（Smoke）
- 关键链路至少一条“从 UI → 网络 → 设备 → UI 回显”的闭环验证

**在 Tenco 中对应**

见 `docs/02_Build_and_Run.md` 的“冒烟测试清单”（建议你跑完再提交流程）。

---

## 5. 代码评审与合并（Code Review / Merge）

**企业做法**

- 通过 PR/MR 进行评审与门禁（CI 必须绿）
- 约定分支策略与提交规范

**在 Tenco 中对应**

- 分支与提交规范建议：`docs/07_Dev_Workflow.md`

**你是个人仓库也建议这么做的原因**

- 你未来会“忘记当时为啥这么改”；PR 描述就是你的“决策记录”
- 即使只有你一个人，CI 也能替你发现“干净环境不通过”

---

## 6. 测试体系（单测/集成测试）

**企业做法**

- 单元测试（UT）：验证算法与纯逻辑（稳定、快）
- 集成测试（IT）：验证模块协作（网络、线程、序列化）
- 手工测试（Manual）：UI/设备联调不可避免，但要有固定 checklist

**在 Tenco 中对应（当前）**

- 已有 UT：
  - `ConfigManager`：配置解析、默认值、轮询间隔边界（`tests/test_configmanager.cpp`）
  - `RouteFollower`：关键轨迹跟随逻辑（`tests/test_routefollower.cpp`）
  - `RoutePathFinder`：路径搜索代价与连通性（`tests/test_routepathfinder.cpp`）
  - `StatusProtocol`：状态地址映射与请求构造（`tests/test_statusprotocol.cpp`）
  - `MapDocument`：地图 JSON 编解码与校验（`tests/test_mapdocument.cpp`）
- 下一步优先补齐：
  - `Map` UI 端到端回归（文件读写 + 场景恢复 + 交互链路）

落地建议见：`docs/08_Testing_and_CI.md`

---

## 7. CI（持续集成）与质量门禁

**企业做法**

- 每次 push/PR 自动：编译（Debug/Release）、跑单测、跑静态检查、产出可执行包

**在 Tenco 中对应（建议目录）**

- GitHub Actions：`.github/workflows/ci.yml`（已配置，Windows + CMake + CTest）
- 测试工程：`tests/`（已配置）

落地建议见：`docs/08_Testing_and_CI.md`

---

## 8. 版本、打包与发布（Release）

**企业做法**

- 语义化版本（SemVer）或企业内部版本规则
- 打 tag、生成 Release Notes、产出安装包/压缩包
- Windows 常用 `windeployqt` 收集依赖

**在 Tenco 中对应**

- 发布说明建议：`docs/09_Release_and_Deployment.md`

---

## 9. 运维与可观测性（日志/崩溃/性能）

**企业做法**

- 统一日志：等级、分类、落盘、滚动
- 崩溃收集：minidump/堆栈（Windows）
- 长稳测试：运行数小时/数天观察内存与重连行为

**在 Tenco 中对应（现状）**

- 已有 UI 日志输出：`Home::logMessage()`（`home.cpp`）
- 已有文件日志：`loggingmanager.*`（按天滚动、按大小轮转）

**建议落地（后续）**

- 在 CI/发布版加入日志等级策略和敏感字段脱敏规则
