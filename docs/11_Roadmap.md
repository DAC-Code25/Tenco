# 工程化改进路线图（可选）

本页是“更企业化”的建议清单，不要求一次做完，建议按收益/风险排序逐步推进。

---

## 1) 文档与可交付性（高收益、低风险）

- 完善 `docs/`（本目录已建立）
- 明确冒烟 checklist（已在 `docs/02_Build_and_Run.md` 给出）

---

## 2) 代码结构（中收益、中风险）

把扁平目录整理为企业常见结构：

```
src/
include/
resources/
tests/
docs/
```

收益：

- 清晰、可扩展、方便做模块化与单测

风险：

- 需要同步修改 `Tenco.pro` 的 SOURCES/HEADERS/INCLUDEPATH

---

## 3) 测试与 CI（高收益、中风险）

- 建 `tests/`，先覆盖：
  - `RouteFollower`
  - `ConfigManager`
  - `Map` 序列化
- 上 GitHub Actions（Windows 最少编译 + 跑测试）

---

## 4) 日志与稳定性（高收益、中风险）

- 引入 `QLoggingCategory` 做分类日志
- 可选：增加落盘日志（滚动/大小限制）
- 对网络错误做更细的分级与退避重连（Backoff）

---

## 5) 构建系统升级（中收益、高风险）

- 从 qmake 迁移到 CMake（Qt6 推荐）
- 适合在项目稳定后做，避免频繁大规模改动影响联调

