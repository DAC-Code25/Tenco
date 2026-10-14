# 项目统一规范入口

本仓库与其他独立子项目共同组成工控机/Tenco/相机服务工作空间。

1. 完整工作空间中，读取 [根 AGENTS](../AGENTS.md)，再按其顺序读取根 `docs/` 的当前状态、问题、变更、会话和总览。
2. 独立克隆时，读取 [统一规范快照](docs/workspace/AGENTS.md) 与 [文档入口](docs/workspace/docs/README.md)；该快照注明日期、会话及各仓功能基线，不证明其他仓库或实车当前状态。
3. 本文件只提供入口，不另定义控制、Git、构建或记录规则。所有约束编辑源是工作空间根 AGENTS。
4. `docs/workspace/` 由根 `tools/project_docs.py sync` 生成，禁止独立手改；独立克隆检查使用 `python docs/workspace/tools/project_docs.py check --documents-only`。

入口生成标记：workspace-docs-entry-v1。
