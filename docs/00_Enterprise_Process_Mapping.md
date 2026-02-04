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
- 缺失：明确的“验收清单/冒烟用例”

**建议落地（优先级高）**

- 在 `docs/02_Build_and_Run.md` 增加“冒烟测试清单”（无需自动化也要可重复）

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

- 明确构建系统（qmake/CMake）与依赖版本（Qt 版本、编译器）
- 工程可重复构建：一台新机器按文档能编译
- 最小化“机器差异”（环境变量、PATH、Kit）

**在 Tenco 中对应**

- 工程文件：`Tenco.pro`
- Qt 模块依赖：`QT += network websockets widgets ...`
- 资源：`resources.qrc` / `mainwindow.ui` / `app_icon.rc`
- 忽略规则：`.gitignore`

**现状**

- qmake 工程清晰（`Tenco.pro`）
- 已开启编译告警：`CONFIG += warn_on`（比 `warn_off` 更企业化）

**建议落地（中优先级）**

- 长期：迁移到 CMake（Qt 6 官方更推荐），但不是必须；先把 CI 跑起来更重要

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

- 当前：几乎没有自动化测试
- 最适合先写 UT 的点：
  - `ConfigManager`：配置解析、默认值、轮询间隔边界（`configmanager.*`）
  - `RouteFollower`：角度归一化、到达判定、速度限幅（`routefollower.*`）
  - `Map` 序列化：`serializeMap()/deserializeMap()`（`map.cpp`）

落地建议见：`docs/08_Testing_and_CI.md`

---

## 7. CI（持续集成）与质量门禁

**企业做法**

- 每次 push/PR 自动：编译（Debug/Release）、跑单测、跑静态检查、产出可执行包

**在 Tenco 中对应（建议目录）**

- GitHub Actions：`.github/workflows/ci.yml`（当前未配置）
- 测试工程：`tests/`（当前未配置）

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

- 当前日志主要输出到 UI：`Home::logMessage()`（`home.cpp`）

**建议落地（后续）**

- 增加 `QLoggingCategory` + 文件日志（可选）并在 CI/发布版设置等级

