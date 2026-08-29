# 构建、运行与调试

## 1. 固定构建依赖

- Qt 6.5.3：`D:/Qt/6.5.3/mingw_64`
- Qt 模块：`Core`, `Gui`, `Widgets`, `Network`, `WebSockets`, `Sql`；关于页三维能力还会按可用性加载 `Quick`, `QuickWidgets`, `Quick3D`, `Quick3DAssetUtils`
- CMake：`D:/Qt/Tools/CMake_64/bin/cmake.exe`
- Ninja：`D:/Qt/Tools/Ninja/ninja.exe`
- MinGW 11.2 64-bit：`D:/Qt/Tools/mingw1120_64`
- C 编译器：`D:/Qt/Tools/mingw1120_64/bin/gcc.exe`
- C++ 编译器：`D:/Qt/Tools/mingw1120_64/bin/g++.exe`

工程入口：`CMakeLists.txt`

以上是用户在 Qt Creator 中确认并要求后续固定使用的本地验证环境。CI 可以继续使用工作流声明的 Qt 版本做额外兼容性验证，但不能替代本机固定 Kit 验收。

---

## 2. 使用 Qt Creator 构建运行（推荐）

1. Qt Creator 打开 `CMakeLists.txt`
2. 选择 `Desktop Qt 6.5.3 MinGW 64-bit`
3. Active build configuration 选择 `Debug`
4. 构建目录设置为 `D:\QTProject\Tenco\build\Desktop_Qt_6_5_3_MinGW_64_bit-Debug`
5. 确认生成器为 Ninja，`BUILD_TESTING=ON`、`TENCO_BUILD_TESTS=ON`
6. QML debugging and profiling 选择 `Enable`
7. Build → Run

注意：CMake 已设置统一输出目录：

- 可执行文件默认在 `..\bin\`（项目上级目录）里

---

## 3. 固定 Kit 的命令行等价构建

以下命令与当前 Qt Creator Kit 的关键配置等价，自动化代理完成修改后必须依次执行配置、完整构建和测试。

```powershell
$env:PATH = 'D:/Qt/6.5.3/mingw_64/bin;D:/Qt/Tools/mingw1120_64/bin;D:/Qt/Tools/Ninja;' + $env:PATH

& 'D:/Qt/Tools/CMake_64/bin/cmake.exe' `
  -S 'D:/QTProject/Tenco' `
  -B 'D:/QTProject/Tenco/build/Desktop_Qt_6_5_3_MinGW_64_bit-Debug' `
  -G Ninja `
  -DCMAKE_MAKE_PROGRAM:FILEPATH='D:/Qt/Tools/Ninja/ninja.exe' `
  -DCMAKE_BUILD_TYPE:STRING=Debug `
  -DBUILD_TESTING:BOOL=ON `
  -DTENCO_BUILD_TESTS:BOOL=ON `
  -DCMAKE_PREFIX_PATH:PATH='D:/Qt/6.5.3/mingw_64' `
  -DCMAKE_C_COMPILER:FILEPATH='D:/Qt/Tools/mingw1120_64/bin/gcc.exe' `
  -DCMAKE_CXX_COMPILER:FILEPATH='D:/Qt/Tools/mingw1120_64/bin/g++.exe' `
  '-DCMAKE_CXX_FLAGS:STRING=-DQT_QML_DEBUG'

& 'D:/Qt/Tools/CMake_64/bin/cmake.exe' `
  --build 'D:/QTProject/Tenco/build/Desktop_Qt_6_5_3_MinGW_64_bit-Debug' `
  --parallel

& 'D:/Qt/Tools/CMake_64/bin/ctest.exe' `
  --test-dir 'D:/QTProject/Tenco/build/Desktop_Qt_6_5_3_MinGW_64_bit-Debug' `
  --output-on-failure
```

`BUILD_TESTING` 是 CTest 总开关，`TENCO_BUILD_TESTS` 是本项目的测试开关，两者必须同时开启。

首行 `PATH` 注入不可省略：Qt Creator 会自动提供 Kit 运行环境，但普通 PowerShell 不会。缺少 Qt/MinGW 运行时路径时，测试进程通常会以 Windows `0xc0000135` 在断言执行前退出。

预设仍可用于补充验证或 CI，但不替代以上固定 Kit：

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
