# Home / Map 可维护性优化 V2 方案

## 目标

本轮继续从 `master` 最新架构出发，进一步降低 `home.cpp` 与 `map.cpp` 的状态复杂度和工具函数堆叠。V2 不做 UI 重写，而是抽取低耦合、可单测、可持续演进的协作者。

## 拆分边界

### 1. HomeManualInputState

职责：

- 管理手动底盘输入的按钮/键盘按住状态。
- 统一判断某个方向在当前授权开关下是否生效。
- 提供 `clearMotionInputs()`，供急停、断连、安全停车清空输入状态。

收益：

- `Home` 不再持有 8 个手动运动 bool。
- 按钮/键盘状态语义集中，后续可继续接入手柄或远程控制源。

### 2. HomeGimbalKeyState

职责：

- 管理云台键盘方向键和 Ctrl 组合键状态。
- 把当前键盘组合解析成明确动作：升降、偏航、俯仰。
- 记录当前正在执行的键盘动作，便于动作切换时先停止旧动作。

收益：

- `Home` 不再持有云台键盘组合状态。
- 云台键盘映射可单测，避免组合键逻辑散落在 UI 事件函数中。

### 3. MapGeometry

职责：

- 提供角度归一化、地图/标准坐标转换、点相等判断、polyline 长度、数字格式化等纯工具。

收益：

- `map.cpp` 的匿名 namespace 减少纯工具函数堆叠。
- 路线规划、地图文档、渲染后续可复用同一套几何基础。

## 测试策略

- 新增 `tenco_test_homeinputstate`，覆盖手动输入状态和云台键盘组合。
- 新增 `tenco_test_mapgeometry`，覆盖角度归一化、坐标转换、polyline 长度和格式化。
- 保留全量 `ctest`，确保 Map 半集成、运动安全层、视频 worker、配置、数据库等测试不回归。

## 后续演进

V3 可以继续拆：

- `HomeCameraCoordinator`
- `HomeGimbalCoordinator`
- `MapSceneRenderer`
- `MapRouteDispatcher`
- `MapRowWorkController`

