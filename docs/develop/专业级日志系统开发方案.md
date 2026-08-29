# 专业级日志系统开发方案

## 1. 背景与现状

当前项目已有基础应用日志能力：

- `LoggingManager` 在程序启动时安装 Qt 消息处理器，将 `qDebug/qInfo/qWarning/qCritical/qFatal` 和 `QLoggingCategory` 输出写入控制台与文件。
- 日志目录位于 `QStandardPaths::AppDataLocation/logs`，当前日志文件为 `tenco.log`。
- 日志按大小轮转：超过 `2MB` 后保留 5 个历史文件。
- 部分网络、视频、云台、底盘、相机、配置模块已使用 `QLoggingCategory`。
- 首页、帮助页、关于页已经能引导用户定位日志目录。

但它还不是生产级日志体系。主要不足：

- 日志策略不可配置，不能通过配置文件控制等级、分类、文件大小和保留数量。
- 文件名固定，不能按日期/会话区分，排查现场问题时不够清晰。
- 日志格式缺少统一上下文字段，如模块、线程、源码位置、会话 ID、运行时长。
- 没有敏感信息脱敏，`authToken`、URL 参数、路径等存在泄露风险。
- 没有操作审计日志，配置修改、任务启动/停止、云台控制、地图文件操作等关键动作无法独立追踪。
- 没有日志导出能力，现场提交问题时仍需要人工找目录、挑文件、打包。
- 没有明确的事件码/错误码体系，不利于长期维护和统计。
- UI 日志、文件日志和模块日志之间缺少统一规则。

因此本分支目标不是“再打印更多日志”，而是把日志系统升级为可维护、可配置、可导出、可审计的生产级基础设施。

## 2. 开发目标

本次开发目标分为四层：

1. 日志基础设施升级

- 支持配置化日志等级。
- 支持配置化文件大小、保留数量、控制台输出开关、源码位置开关。
- 支持按会话启动生成运行日志，保留当前 `tenco.log` 兼容入口。
- 支持分类日志过滤规则。
- 统一日志格式，包含时间、等级、分类、线程、会话 ID、运行时长和消息内容。

2. 安全与可维护性

- 对敏感字段进行脱敏，包括 `authToken`、`Authorization`、`token`、`password`、`secret` 等。
- 对 URL 中的 query 参数和认证头做保守脱敏。
- 避免高频状态轮询、视频重连等模块刷屏。
- 明确 DEBUG 日志在发布版默认关闭。

3. 运维与审计

- 新增操作审计日志接口，记录关键用户操作和系统动作。
- 支持单独审计文件 `audit.log`，便于追踪“谁在什么时候做了什么”。
- 维护界面支持查看和调整日志参数。
- 帮助/关于/维护界面统一展示日志目录和导出入口。

4. 现场问题交付

- 支持一键导出诊断包。
- 诊断包包含运行日志、审计日志、当前配置、备份配置列表、版本信息和环境摘要。
- 导出时默认脱敏配置中的敏感字段。

## 3. 非目标

第一版不做以下内容：

- 不接入远程日志平台。
- 不上传日志到服务器。
- 不做 Windows minidump 崩溃转储。
- 不做性能火焰图或完整 tracing 系统。
- 不引入第三方日志库，优先使用 Qt 自身能力和现有工程结构。

这些能力可以作为第二阶段扩展。

## 4. 配置设计

在 `config.json` 新增 `logging` 段：

```json
{
  "logging": {
    "level": "info",
    "consoleEnabled": true,
    "fileEnabled": true,
    "includeSourceLocation": false,
    "includeThreadId": true,
    "includeCategory": true,
    "maxFileBytes": 5242880,
    "maxBackupFiles": 10,
    "perSessionFile": true,
    "auditEnabled": true,
    "auditMaxFileBytes": 5242880,
    "auditMaxBackupFiles": 10,
    "redactSensitiveData": true,
    "categoryRules": [
      "tenco.net.status.debug=false",
      "tenco.gimbal.control.debug=false"
    ]
  }
}
```

字段含义：

- `level`：全局最低等级，支持 `debug/info/warn/error/fatal`。
- `consoleEnabled`：是否输出到 Qt Creator/终端。
- `fileEnabled`：是否写入运行日志文件。
- `includeSourceLocation`：是否记录文件名、行号、函数名。
- `includeThreadId`：是否记录线程 ID，便于排查多线程网络与视频问题。
- `includeCategory`：是否记录 `QLoggingCategory` 分类。
- `maxFileBytes`：单个运行日志最大大小。
- `maxBackupFiles`：运行日志最多保留数量。
- `perSessionFile`：是否按启动会话生成文件，如 `tenco_20260522_142300.log`。
- `auditEnabled`：是否启用操作审计日志。
- `auditMaxFileBytes`：单个审计日志最大大小。
- `auditMaxBackupFiles`：审计日志最多保留数量。
- `redactSensitiveData`：是否开启敏感信息脱敏。
- `categoryRules`：Qt 分类日志过滤规则，最终应用到 `QLoggingCategory::setFilterRules()`。

默认策略：

- Debug 构建默认 `level=debug`。
- Release 构建默认 `level=info`。
- 发布现场默认保留 10 个运行日志和 10 个审计日志。
- 状态轮询类 DEBUG 默认关闭。

## 5. 文件结构设计

日志目录：

```text
<AppDataLocation>/logs/
  tenco.log
  tenco.log.1
  tenco_20260522_142300.log
  tenco_20260522_142300.log.1
  audit.log
  audit.log.1
  diagnostics/
    tenco_diag_20260522_143000/
```

说明：

- `tenco.log` 继续保留，作为兼容入口，便于用户和帮助文档查找。
- 开启 `perSessionFile` 后，同时写入会话日志文件。
- `audit.log` 只记录关键操作和配置变更摘要，不记录高频状态。
- `diagnostics` 存放一键导出的诊断包。

## 6. 日志格式设计

运行日志建议格式：

```text
2026-05-22 14:23:01.123 [INFO] [tenco.gimbal.control] [tid=0x4a20] [sid=20260522_142300] [uptime=00:00:31.042] PLC connected host=192.168.31.120 port=502
```

源码位置开启时：

```text
2026-05-22 14:23:01.123 [WARN] [tenco.net.status] [tid=0x3f10] [sid=20260522_142300] [homenetworkworker.cpp:136] Status poll failed http=0 error="Operation canceled"
```

审计日志建议格式：

```text
2026-05-22 14:23:30.405 [AUDIT] action=config.apply result=success actor=local_ui target=config.json summary="保存并应用维护配置"
2026-05-22 14:24:02.003 [AUDIT] action=gimbal.jog result=accepted actor=keyboard target=pitch direction=up
```

## 7. 代码改造方案

### 7.1 `LoggingManager`

扩展为真正的日志服务：

- 新增 `LoggingManager::Settings`。
- 支持 `initialize(settings)`。
- 支持 `reconfigure(settings)`，在维护页保存并应用后热更新日志策略。
- 支持 `logDirectoryPath()`。
- 支持 `currentLogFilePath()`。
- 支持 `auditLogFilePath()`。
- 支持 `exportDiagnostics(targetPath, options)`。
- 支持 `audit(action, result, fields)`。

内部机制：

- 使用 `QMutex` 保护文件句柄。
- 运行日志和审计日志分离。
- 每次写入前检查文件大小并轮转，避免长时间运行超过大小限制。
- 消息处理器中先脱敏再输出。
- 统一将 Qt 日志等级映射为 `DEBUG/INFO/WARN/ERROR/FATAL`。

### 7.2 `ConfigManager`

新增：

- `struct LoggingConfig`。
- `ConfigSnapshot::logging`。
- `logging()` 访问器。
- JSON 读取、写入、默认值、sanitize。

热加载逻辑：

- 维护页执行“保存并应用”后，`ConfigManager::configChanged()` 发出。
- 主窗口或启动入口监听配置变化，调用 `LoggingManager::reconfigure(ConfigManager::instance().logging())`。

### 7.3 维护界面

新增“日志”配置页：

- 全局最低日志级别。
- 控制台输出开关。
- 文件输出开关。
- 源码位置开关。
- 线程 ID 开关。
- 单文件大小。
- 保留文件数。
- 会话日志开关。
- 审计日志开关。
- 脱敏开关。
- 分类过滤规则编辑。
- 打开日志目录。
- 导出诊断包。

交互要求：

- 修改日志参数后，保存并应用应立即生效。
- 不需要重启程序。
- 分类规则输入错误时应提示，不应破坏现有日志。
- 导出诊断包成功后给出路径。

### 7.4 帮助页与关于页

同步说明：

- 帮助页“数据与日志”补充日志等级、运行日志、审计日志、诊断包说明。
- 关于页运行信息显示当前日志级别、当前日志文件、审计日志文件和诊断包目录。

### 7.5 模块日志规范

统一模块分类命名：

- `tenco.main`
- `tenco.config`
- `tenco.ui.home`
- `tenco.ui.map`
- `tenco.ui.maintenance`
- `tenco.net.status`
- `tenco.net.chassis`
- `tenco.video.stream`
- `tenco.video.oak`
- `tenco.gimbal.control`
- `tenco.rowwork.client`
- `tenco.map.document`
- `tenco.route.follow`

规范要求：

- 高频轮询成功不打印 INFO。
- 状态变化打印 INFO。
- 可恢复错误打印 WARN。
- 数据损坏、配置不可用、文件保存失败打印 ERROR。
- 用户操作同时写 UI 日志和审计日志。
- 不直接打印 token、完整认证头和敏感 query。

## 8. 诊断包设计

导出文件名：

```text
tenco_diag_yyyyMMdd_HHmmss.zip
```

内容：

```text
diagnostics.json
logs/
  tenco.log
  audit.log
  latest session logs...
config/
  config.redacted.json
  backup_index.txt
runtime/
  version.txt
  environment.txt
```

`diagnostics.json` 内容：

- 应用版本。
- Qt 版本。
- 构建类型。
- 运行目录。
- 配置文件路径。
- 日志目录。
- 当前日志策略。
- 当前视频后端。
- 当前网络地址脱敏摘要。
- 当前云台/行作业启用状态。

第一版按目录导出诊断包，避免依赖 Qt 私有 zip writer；后续如需要可再增加 zip 压缩。

## 9. 安全与脱敏规则

需要脱敏的内容：

- `Authorization: Bearer xxx`
- `authToken`
- `token`
- `password`
- `secret`
- URL query 中的 `token/password/key/secret`

脱敏格式：

```text
Authorization: Bearer ***REDACTED***
"authToken":"***REDACTED***"
http://host/path?token=***REDACTED***
```

默认开启脱敏。

## 10. 开发任务清单

### 阶段一：基础日志能力升级

- [x] 新增 `LoggingManager::Settings`。
- [x] 支持配置化日志等级。
- [x] 支持配置化控制台/文件输出。
- [x] 支持运行日志大小轮转参数。
- [x] 支持线程 ID、分类、源码位置开关。
- [x] 支持运行中 `reconfigure()`。
- [x] 日志脱敏函数。

### 阶段二：配置接入

- [x] `ConfigManager` 新增 `LoggingConfig`。
- [x] `config.json` 新增 `logging` 默认段。
- [x] 保存、加载、备份、恢复逻辑覆盖 logging 段。
- [x] `ConfigManager::configChanged()` 后热应用日志配置。

### 阶段三：审计日志

- [x] 新增 `LoggingManager::audit()`。
- [x] 审计日志独立文件和轮转。
- [x] 维护页保存/应用/恢复/加载配置写审计。
- [x] 首页云台控制、底盘控制、急停、任务启动/停止写审计。
- [x] 地图保存、打开、新建等文件操作写审计。

### 阶段四：维护界面接入

- [x] 维护页新增“日志”配置组。
- [x] 日志参数带说明文本。
- [x] 保存并应用后日志策略立即生效。
- [x] 打开日志目录按钮。
- [x] 导出诊断包按钮。

### 阶段五：诊断包

- [x] 收集日志文件。
- [x] 收集脱敏配置。
- [x] 生成环境摘要。
- [x] 导出目录。
- [x] UI 给出导出结果路径。

### 阶段六：文档与验收

- [x] 更新帮助页“数据与日志”说明。
- [x] 更新关于页运行信息。
- [x] 更新 README 工程化增强说明。
- [x] 更新本开发方案状态。
- [x] 编译运行验证。

## 11. 验收标准

- 程序启动后生成运行日志。
- 日志包含时间、等级、分类、线程、会话信息。
- 日志等级改为 `warn` 后，`debug/info` 不再写入。
- 维护页修改日志设置并“保存并应用”后无需重启即可生效。
- 运行日志超过配置大小后自动轮转。
- 审计日志能记录配置保存、保存并应用、恢复备份和关键控制操作。
- 日志中不会出现明文 `authToken` 或 `Authorization`。
- 点击导出诊断包后，能生成包含日志、脱敏配置和环境信息的包或目录。
- 帮助页和关于页能正确展示日志相关说明和路径。
- 高频状态轮询不刷屏。
- Qt Creator Debug 构建通过。

## 12. 风险与处理

- 风险：Qt 消息处理器中做太多工作可能阻塞主线程。
  - 处理：第一版保持同步写文件但控制单行处理复杂度；后续可引入异步队列。

- 风险：日志热重载时正在写文件。
  - 处理：`QMutex` 保护关闭、重开和写入。

- 风险：脱敏误伤普通文本。
  - 处理：只对明确字段名和认证头做脱敏，不做过度替换。

- 风险：`QZipWriter` 不可用。
  - 处理：第一版允许导出为目录，后续再补 zip 压缩。

- 风险：维护页参数过多导致复杂。
  - 处理：默认只展示常用项，高级项折叠或单独卡片。

## 13. 当前开发状态

已完成：

- 新建日志系统开发分支。
- 完成专业级日志系统开发方案。
- 改造 `LoggingManager`，支持配置化日志等级、文件/控制台输出、会话日志、轮转、脱敏、审计日志和诊断目录导出。
- `ConfigManager`、`config.json`、维护页、帮助页和关于页已接入日志配置与路径展示。
- 维护页已提供日志参数编辑、打开日志目录、导出诊断包和配置操作审计。
- 已在 Qt Creator 6.5.3 Debug 构建目录完成编译验证。
- README 已补充日志系统、维护页日志配置和诊断包导出说明。
- 首页云台点动、停止、软限位拒绝、底盘按钮/键盘手动运动、急停已写入审计日志。
- 地图新建、加载、保存/另存为，以及普通路线开始/暂停/恢复/停止已写入审计日志。
- 直线作业计划下发、开始、暂停、恢复、停止、成功/失败回调已写入审计日志。

后续可选增强：

- 可继续补充更细粒度的地图编辑审计，如点/路径增删改、批量生成点、直线作业中间点编辑等。
- 如现场需要集中留痕，可在第二阶段接入远程日志平台或压缩诊断包导出。
