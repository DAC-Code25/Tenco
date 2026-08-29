# Tenco 开发文档（随代码版本化）

本目录用于存放“企业协作/交付友好”的项目文档：和代码一起提交、随版本演进、保证新人拉代码即可按文档完成构建/运行/联调。

开始任何需要修改代码的任务前，必须先完整阅读仓库根目录的标准规范 `AGENTS.md`。

如果你是以“学习”为主，建议同时打开 `D:\QTProject\Tenco-learning\README.md`，它提供了更偏教学的阅读路线与练习任务（不建议放进生产仓库）。

## 推荐阅读顺序

1. `00_Enterprise_Process_Mapping.md`：企业开发流程 → 本项目逐步落地映射（总纲）
2. `01_Project_Overview.md`：项目定位、功能范围、运行环境
3. `02_Build_and_Run.md`：构建/运行/调试
4. `03_Configuration.md`：`config.json` 与 `ConfigManager`
5. `04_Architecture.md`：模块划分、线程模型、核心数据流
6. `05_Modules.md`：主要类与关键接口（按文件）
7. `06_Network_and_Protocol.md`：HTTP/WebSocket/MJPEG 协议与字段
8. `07_Dev_Workflow.md`：企业开发流程（分支/提交/评审/规范）
9. `08_Testing_and_CI.md`：单测策略与 CI 落地建议
10. `09_Release_and_Deployment.md`：版本/打包/发布
11. `10_Troubleshooting.md`：常见问题排查
12. `11_Roadmap.md`：工程化改进路线图（可选）
13. `12_AI_Project_Learnings.md`：工程经验总结（带证据链）
14. `13_Optimization_Audit_2026-03-12.md`：全面审查与后续优化清单
15. `develop/数据库系统开发方案.md`：数据库选型、分层、迁移、同步与时序方案
16. `develop/统一运动安全层开发方案.md`：统一运动安全层设计、仲裁规则、集成点和测试矩阵
17. `develop/MJPEG视频工作线程管线开发方案.md`：MJPEG worker 化、最新帧、丢帧限帧、录像与指标方案
18. `develop/核心架构与性能优化V1方案.md`：核心架构与性能优化 V1，覆盖模块化、公共网络策略、配置治理、异步日志和低风险职责拆分
19. `develop/首页与地图可维护性优化V2方案.md`：Home/Map 可维护性 V2，覆盖输入状态、云台键盘状态和地图几何工具抽取

## 专题开发方案

- `develop/关于界面开发方案.md`：关于页产品信息、诊断信息和三维车辆模型展示方案。
- `develop/帮助界面开发方案.md`：帮助页分类导航、现场操作、安全提示和文档入口方案。
- `develop/首页云台控制开发方案.md`：PLC Modbus、云台/升降控制、快捷键和安全限位方案。
- `develop/首页与地图可维护性优化V2方案.md`：继续降低 `home.cpp` / `map.cpp` 状态复杂度，抽出可单测输入状态与地图几何工具。
- `develop/多垄穿梭作业闭环系统开发方案.md`：多垄任务模型、上位机/工控机边界、闭环控制和鲁棒性方案。
- `develop/数据库系统开发方案.md`：数据库选型、分层、迁移、同步与时序方案。
- `develop/统一运动安全层开发方案.md`：统一运动安全层设计、仲裁规则、集成点和测试矩阵。
- `develop/维护界面配置管理开发方案.md`：配置保存、恢复、热加载和生产维护方案。
- `develop/专业级日志系统开发方案.md`：日志分类、轮转、脱敏、审计和诊断包方案。
- `develop/核心架构与性能优化V1方案.md`：CMake 内部库拆分、协作者抽取、网络公共策略、配置治理和异步日志方案。
- `develop/MJPEG视频工作线程管线开发方案.md`：MJPEG worker 化、最新帧、丢帧限帧、录像与指标方案。
- `develop/USB_OAK相机接入开发方案.md`：11F1E2 + OAK-D-Pro-W USB 相机接入、工控机部署和稳定性方案。
- `develop/早期路线执行链路改造方案.md`：早期配置、坐标、路线执行和停止逻辑历史方案归档。
- `develop/项目开发流程与文档治理方案.md`：Git 基线、分支、中文交付、记录和固定 Qt 构建环境治理方案。
