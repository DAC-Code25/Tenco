# 开发流程与规范（面向企业习惯）

你是个人维护也建议遵循“企业流程”的原因：这套流程本质是为了降低返工、提升可追溯性、让项目长期可维护。

---

## 1. 分支策略（建议）

### 简化版（个人仓库够用）

- `master`（或 `main`）保持随时可运行
- 每个改动建一个分支：
  - `feature/<topic>`：新增功能
  - `fix/<bug>`：修复问题
  - `refactor/<area>`：重构

合并回主分支前，确保至少：

- 本地编译通过
- 冒烟 checklist 通过（见 `docs/02_Build_and_Run.md`）

---

## 2. 提交（Commit）规范（建议）

每次提交聚焦一个主题，消息格式推荐：

- `feat: ...` 新增功能
- `fix: ...` 修复 bug
- `refactor: ...` 重构但不改行为
- `docs: ...` 文档
- `chore: ...` 杂项（依赖/脚本）

例：

- `fix: include QNetworkReply to satisfy moc`
- `refactor: split Home networking into StatusClient/ChassisClient/VideoClient`

---

## 3. PR / Code Review（建议你“自我评审”）

即使没有同事，也建议你每次合并前写一段 PR 描述（可以只写在 GitHub 上）：

- 背景：为什么要改
- 改了什么：关键点列表
- 风险：可能影响哪些功能
- 验证：跑了哪些测试/冒烟步骤

这样未来回头看会非常省时间。

---

## 4. 代码规范（建议）

### 4.1 命名与结构

- 类职责单一：模块拆分（本项目已开始做）
- 文件命名统一：`xxxclient.*`、`xxxmanager.*`、`xxxview.*`

### 4.2 线程与 QObject

- worker 放到独立线程（例如 `HomeNetworkWorker`）
- UI 更新必须回到 UI 线程（当前通过信号/槽自然做到）

### 4.3 配置与常量

- 环境相关参数必须进 `config.json`
- 协议字段建议集中管理（后续可引入 `protocol/` 目录）

---

## 5. 代码结构的下一步“企业化”

当前仓库文件仍较扁平，企业里常见结构：

```
docs/
src/
include/
tests/
resources/
tools/   (脚本、打包、格式化)
```

是否要做这一步，见 `docs/11_Roadmap.md` 的建议路线。

