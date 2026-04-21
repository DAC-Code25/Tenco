# 构建、运行与调试

## 1. 构建依赖

- Qt 6（推荐 Qt 6.5+）
- Qt 模块：`Core`, `Gui`, `Widgets`, `Network`, `WebSockets`
- CMake 3.16+
- 编译器：
  - Windows：MinGW 或 MSVC（以你安装的 Qt Kit 为准）

工程入口：`CMakeLists.txt`

---

## 2. 使用 Qt Creator 构建运行（推荐）

1. Qt Creator 打开 `CMakeLists.txt`
2. 选择 Kit（例如 Desktop Qt 6.8.3 MinGW 64-bit）
3. Build → Run

注意：CMake 已设置统一输出目录：

- 可执行文件默认在 `..\bin\`（项目上级目录）里

---

## 3. 命令行构建（可选）

前提：`cmake`、编译器、Qt 工具链可用。

```powershell
cmake -S . -B build\cmake -G Ninja -DTENCO_BUILD_TESTS=ON
cmake --build build\cmake --config Debug -j
ctest --test-dir build\cmake -C Debug --output-on-failure
```

也可以使用预设（推荐）：

```powershell
cmake --preset default
cmake --build --preset default
ctest --preset default
```

---

## 4. 运行前必须改的配置

运行时会读取 `config.json`（搜索顺序见 `ConfigManager`，也见 `docs/03_Configuration.md`）。

你至少需要确认：

- `network.websocketUrl`
- `network.statusReadUrl`
- `network.writeInsUrl`
- `network.saveFileUrl`
- `video.streamUrl`

否则：状态不会刷新/控制发不出去/视频打不开。

可选：通过命令行显式指定配置文件（便于多环境切换）：

```powershell
.\Tenco.exe --config D:\QTProject\Tenco\config.json
```

---

## 5. 冒烟测试清单（建议每次改动后都跑）

> 目的：用最少步骤验证关键链路没有被改坏（企业里这份 checklist 很重要）。

### 5.1 基础启动

- [ ] 程序能启动且无崩溃
- [ ] 首页 UI 元素正常显示（日志框、视频区域、按钮等）

### 5.2 状态轮询（HTTP）

- [ ] “通信正常/通信故障”按钮能随网络变化切换
- [ ] 电量/电压/温度/模式/位姿/地图名等至少 2~3 项在变化（证明解析 OK）
- [ ] 若后端不可达，日志可解释地出现 `http=0` / `Operation canceled`，程序本身不崩溃

### 5.3 手动控制（WebSocket）

- [ ] WebSocket 连接成功（日志提示）
- [ ] 按钮长按能持续发送速度（松开能停）
- [ ] 键盘 W/A/S/D 控制（松开能停）
- [ ] 急停能清零速度

### 5.4 视频（MJPEG）

- [ ] 选择话题后开始显示画面（或至少提示“已连接/等待”）
- [ ] 断网后能提示并自动重连
- [ ] 录像：开始/停止后能生成 `.mjpeg` 文件
- [ ] 截图：能生成 `.jpg` 文件

### 5.5 地图/路线闭环

- [ ] 地图能生成网格、添加点、添加路径
- [ ] 保存地图为 JSON、重新加载后数据一致
- [ ] “新建”按钮满足三种分支：空地图直接提示；未保存新建地图时提示“保存/不保存”；已加载地图时提示保存当前修改
- [ ] 路线开始后能分段派发、收到段完成信号继续下一段/结束
- [ ] 路线停止能立即停速度
- [ ] 跨点路线添加符合有向路径规则：只有路径方向连续可达时，`起点 -> 终点` 才允许加入路线队列
