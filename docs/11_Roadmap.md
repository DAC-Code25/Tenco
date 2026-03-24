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

当前进展：

- [x] 已抽离 `home_status_presenter.*`（状态解析与状态区 UI 映射）
- [ ] 继续拆分 `Home`（控制/媒体/GPS 提交）
- [ ] 继续拆分 `Map`（UI 构建/调度/渲染分层）

风险：

- 需要同步修改 `CMakeLists.txt` 中的目标源文件与包含路径

---

## 3) 测试与 CI（高收益、中风险）

- [x] 建 `tests/`，已覆盖：
  - [x] `RouteFollower`
  - [x] `ConfigManager`
  - [x] `RoutePathFinder`
  - [x] `StatusProtocol`
  - [x] `MapDocument` 序列化编解码
  - [x] `Map` 路线执行端到端回归
- [x] 上 GitHub Actions（Windows 编译 + 跑测试）

---

## 4) 日志与稳定性（高收益、中风险）

- [x] 引入 `QLoggingCategory` 做分类日志
- [x] 增加落盘日志（滚动/大小限制）
- [x] 网络重连退避（Status/Chassis/Video）

---

## 5) 构建系统持续优化（中收益、中风险）

- [x] 已统一为 CMake（移除旧版构建文件）
- [ ] 增加多平台预设（MSVC/MinGW）
- [x] 增加发布构建预设（RelWithDebInfo/Release）
