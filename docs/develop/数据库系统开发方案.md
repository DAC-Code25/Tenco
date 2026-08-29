# Tenco 数据库系统开发方案

## 1. 结论

对当前 Tenco 这种 Qt 上位机、机器人/底盘联调、现场网络不稳定、需要离线可用的工业项目，推荐采用：

- **客户端本地库：SQLite**
- **中心服务库：PostgreSQL**
- **高频状态/轨迹历史：PostgreSQL + TimescaleDB**

V1 先落地 SQLite 本地数据库，保证单机上位机在没有中心服务器、没有网络、设备服务不稳定时仍能保存地图、任务、运行事件和必要状态缓存。V2 再接 PostgreSQL 做中心化管理、多终端协同、备份、统计和审计。V3 对高频状态、轨迹、设备遥测上 TimescaleDB hypertable，处理按时间范围查询、压缩和保留策略。

本分支已经先加入 `DatabaseManager` 和 `database` 配置段，作为后续持久化开发的基础。

## 2. 为什么这样选

### 2.1 SQLite 适合当前客户端

Tenco 当前是 Qt Widgets 桌面程序，主要运行在操作员机器或工控机旁路机器上。现场软件有几个典型要求：

- 断网时仍能打开地图、编辑任务、记录关键操作。
- 安装部署尽量简单，不能要求每台设备先安装数据库服务。
- 数据量中等，主要是地图 JSON、任务计划、审计事件、少量状态缓存。
- Qt 可以直接通过 `Qt6::Sql` 和 `QSQLITE` 使用本地数据库。

SQLite 是嵌入式单文件数据库，适合本地配置、缓存、任务和操作历史。启用 WAL 后，读写体验更适合桌面程序：UI 读数据和后台写入不会频繁互相卡住。

### 2.2 PostgreSQL 适合中心服务

当项目进入真实生产环境后，仅靠每台上位机各自保存本地文件会有明显问题：

- 多台车、多台上位机、多班组无法统一查看历史。
- 审计、追溯、报表、备份很难做。
- 权限、账号、远程运维、版本迁移需要服务端承载。

PostgreSQL 更适合作为中心库：事务能力强，支持 JSONB，既能保存结构化数据，也能保存当前项目里大量已有的 JSON 文档模型。未来如果有后端服务，建议由后端服务访问 PostgreSQL，Qt 客户端通过 HTTP/gRPC 同步，不直接把数据库公网暴露给桌面端。

### 2.3 TimescaleDB 适合高频时序

状态轮询、车辆位姿、速度、电池、电机温度、故障码、路线执行采样都天然是时间序列。高频数据如果长期塞进普通业务表，会很快影响查询和维护。

TimescaleDB 建在 PostgreSQL 之上，适合把 `telemetry_samples`、`pose_samples`、`route_execution_samples` 这类表升级为 hypertable，并配置：

- 按时间分区
- 自动压缩
- 数据保留策略
- 分钟/小时级聚合视图

## 3. 不推荐的选择

- **MySQL/MariaDB**：业务系统常见，但对当前项目的 JSON 文档、时序扩展、工业追溯分析不是最优。
- **MongoDB**：JSON 文档体验好，但事务、部署、嵌入式离线客户端和 Qt 集成成本不如 SQLite + PostgreSQL。
- **InfluxDB 单独部署**：时序能力强，但会多引入一套数据库体系。除非后续遥测规模很大，否则 TimescaleDB 更利于统一业务数据与时序数据。
- **只用文件 JSON**：当前地图文件可以继续支持导入导出，但不能承担长期审计、查询、版本迁移和多实体关联。

## 4. 数据边界

### 4.1 继续放在 `config.json`

这些属于部署参数，不进数据库主数据：

- 设备 IP、URL、端口
- 视频流地址
- 控制阈值默认参数
- 日志等级与轮转
- 数据库连接信息

原因：程序启动前就需要读取这些内容；数据库不可用时也要能启动到可诊断状态。

### 4.2 进入数据库

V1 优先入库：

- 地图文档：`MapDocument`
- 垄作业计划：`RowWorkPlan`
- 任务编排计划：`RowMissionPlan`
- 关键操作事件：配置保存、开始/暂停/停止路线、急停、云台动作、相机拍照录像
- 低频状态快照：连接状态、设备模式、当前地图名、当前任务状态

V2/V3 再入库：

- 路线执行记录
- 车辆位姿轨迹
- 高频状态采样
- 相机/视频事件
- 故障与告警生命周期

## 5. 当前 V1 表设计

本分支已创建以下基础表：

- `schema_migrations`：数据库 schema 版本
- `app_metadata`：本地键值元信息
- `maps`：地图文档 JSON
- `row_work_plans`：垄作业计划 JSON
- `row_mission_plans`：任务编排计划 JSON
- `operation_events`：关键操作事件与审计入库
- `telemetry_samples`：状态采样入口，V3 可迁移到 TimescaleDB hypertable

SQLite 里 JSON 字段先用 `TEXT` 保存，PostgreSQL 里使用 `JSONB`。这样可以复用当前项目已有的 JSON 编解码逻辑，减少第一阶段改造风险。

## 6. 配置

`config.json` 新增：

```json
"database": {
  "backend": "sqlite",
  "connectionName": "tenco_main",
  "sqliteFilePath": "",
  "host": "",
  "port": 5432,
  "databaseName": "tenco",
  "userName": "",
  "password": "",
  "useWAL": true,
  "foreignKeys": true,
  "synchronousNormal": true,
  "busyTimeoutMs": 5000,
  "cacheSizePages": 2000,
  "connectTimeoutMs": 5000,
  "reconnectIntervalMs": 3000,
  "enableTelemetryTables": true,
  "enableLocalCache": true,
  "enableAuditSync": true
}
```

`sqliteFilePath` 为空时，默认使用：

```text
<appDir>/data/tenco.sqlite3
```

## 7. 代码结构计划

### 7.1 当前已落地

- `databasemanager.h/.cpp`
  - 读取 `ConfigManager::DatabaseConfig`
  - 初始化 SQLite 或 PostgreSQL 连接
  - SQLite 启用 WAL、外键、busy timeout
  - 初始化 schema
  - 提供 schema 版本查询

- `ConfigManager`
  - 新增 `DatabaseConfig`
  - 支持配置读取、保存、边界校验

- `main.cpp`
  - 程序启动时初始化数据库
  - 配置变更后重新初始化数据库
  - 程序退出时关闭数据库

### 7.2 下一步建议新增 Repository 层

建议按业务对象拆分：

- `MapRepository`
- `RowWorkPlanRepository`
- `RowMissionPlanRepository`
- `OperationEventRepository`
- `TelemetryRepository`

UI 和业务模块不直接写 SQL，而是调用 repository。这样后续 SQLite 切 PostgreSQL、表结构迁移、同步策略都不会污染页面代码。

## 8. 同步架构

### V1：本地单机

```text
Qt UI / 业务模块
  -> Repository
  -> DatabaseManager
  -> SQLite
```

### V2：中心服务

```text
Qt 客户端
  -> Local Repository / SQLite
  -> Sync Worker
  -> Tenco Backend API
  -> PostgreSQL
```

客户端仍保留 SQLite，作为离线缓存和本地事务缓冲。中心库由后端服务统一写入，避免多个客户端直接连接数据库造成权限、版本和网络安全问题。

### V3：时序库

```text
状态/轨迹采样
  -> 本地短期 SQLite 缓存
  -> 批量同步
  -> PostgreSQL + TimescaleDB hypertable
```

## 9. 迁移策略

每次 schema 变更都必须：

1. 增加迁移版本号
2. 写幂等迁移 SQL
3. 在 `schema_migrations` 记录版本
4. 单测覆盖旧库升级到新库
5. 禁止在 UI 线程执行大迁移

V1 当前版本为 `1`。

## 10. 备份与恢复

SQLite：

- 启动时可检查数据库存在性与 schema 版本。
- 发布版建议增加“导出诊断包”时包含数据库摘要，不默认包含完整数据库，避免泄露现场数据。
- 可新增 `data_backups/`，按天或版本升级前备份。

PostgreSQL：

- 生产环境使用 `pg_dump`/物理备份/云厂商备份。
- 至少保留每日备份与变更前备份。
- 审计表与任务表要纳入长期保留。

TimescaleDB：

- 高频原始数据按项目要求设置保留期。
- 聚合数据长期保存。

## 11. 安全要求

- 生产环境数据库密码不要提交到仓库。
- 如果客户端直连 PostgreSQL，只能用于内网临时部署，账号必须最小权限。
- 正式企业部署建议客户端只访问后端 API，由服务端持有数据库凭据。
- 操作事件表要复用日志系统的脱敏规则，不保存 token、密码、完整 URL 凭据。

## 12. 测试计划

已新增：

- `tests/test_databasemanager.cpp`：验证 SQLite 初始化、建表和 schema 版本。
- `tests/test_configmanager.cpp`：覆盖数据库配置解析和边界校验。

后续补充：

- 旧 schema 迁移到新 schema
- Repository CRUD
- SQLite 文件路径不可写时的错误处理
- 配置热更新后的重连
- 高频写入压力测试
- 断电/异常退出后的 WAL 恢复测试

## 13. 分阶段开发任务

### Phase 1：SQLite 本地持久化

- [x] 接入 `Qt6::Sql`
- [x] 新增 `DatabaseManager`
- [x] 新增数据库配置
- [x] 初始化基础 schema
- [ ] 新增 Repository 层
- [ ] 地图保存/加载支持数据库入口
- [ ] 作业计划保存/加载支持数据库入口
- [ ] 操作审计同步入 `operation_events`

### Phase 2：运行记录与查询

- [ ] 记录路线执行会话
- [ ] 记录任务开始、暂停、恢复、完成、失败
- [ ] 记录关键状态快照
- [ ] 维护页增加数据库状态和导出入口

### Phase 3：中心 PostgreSQL

- [ ] 设计后端 API
- [ ] PostgreSQL schema 迁移脚本
- [ ] 本地到中心的批量同步 worker
- [ ] 冲突处理：以 UUID、版本号、更新时间和来源设备区分
- [ ] 服务端备份与恢复流程

### Phase 4：TimescaleDB 时序历史

- [ ] 将高频采样表迁移为 hypertable
- [ ] 配置压缩和保留策略
- [ ] 增加轨迹回放和历史曲线查询
- [ ] 增加按任务/设备/时间范围的报表

## 14. 参考资料

- Qt SQL 模块官方文档：https://doc.qt.io/qt-6/qtsql-index.html
- SQLite 官方文档：https://sqlite.org/index.html
- SQLite WAL 官方文档：https://sqlite.org/wal.html
- PostgreSQL JSON 类型官方文档：https://www.postgresql.org/docs/current/datatype-json.html
- PostgreSQL MVCC 官方文档：https://www.postgresql.org/docs/current/mvcc.html
- TimescaleDB hypertables 官方文档：https://docs.timescale.com/use-timescale/latest/hypertables/about-hypertables/
