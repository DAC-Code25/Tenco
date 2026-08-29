# 统一运动安全层开发方案

## 1. 背景与问题

当前首页存在三类速度来源：

- 按钮手动控制：前进、后退、左转、右转按钮按住时周期发送速度。
- 键盘手动控制：`W/A/S/D` 按住时周期发送速度。
- 路线跟随控制：`RouteFollower` 根据位姿和 polyline 输出 `velocityCommand(linear, angular)`。

这些来源最终都会调用 `Home::sendVelocityCommand()`，但安全语义分散在各个 UI 事件中：

- 方向按钮或键盘释放时主要停止各自定时器，未统一下发零速。
- 路线取消、急停、通信断开等场景各自处理，容易遗漏。
- 手动控制和路线控制没有统一仲裁规则，后续扩展自动作业/远程遥控时风险会增加。

机器人上位机需要把“速度发送”视为安全关键链路。`MotionCommandArbiter` 的目标是成为底盘 `cmd_vel` 的唯一出口，集中处理仲裁、心跳、零速和审计。

## 2. 目标

### 2.1 本期目标

1. 新增 UI 无关的 `MotionCommandArbiter` 模块。
2. 将手动按钮、键盘、路线跟随的速度输出汇总到 arbiter。
3. 任一输入释放、手动控制关闭、急停、路线取消、底盘断连、窗口失焦/最小化、状态通信故障时统一下发零速。
4. 保留原有 `manualMotionRepeatIntervalMs` 配置作为手动心跳周期。
5. 为仲裁核心补单元测试，覆盖输入切换、释放停车、路线优先级、急停停车。

### 2.2 非目标

- 不改底盘 WebSocket 协议字段。
- 不重写 `RouteFollower` 的跟随算法。
- 不改变现有 UI 布局。
- 不实现复杂驾驶模式权限系统；本期只落最小可维护安全闭环。

## 3. 设计原则

- 唯一出口：只有 arbiter 直接发出 `velocityCommand(linear, angular)`，`Home` 再转发给 `ChassisClient`。
- 保守停车：不确定、禁用、断连、释放输入时优先发零速。
- 可测试：仲裁规则不依赖 UI 控件、不依赖 WebSocket。
- 可审计：关键安全动作由 `Home` 记录审计，arbiter 提供 reason，避免模块直接依赖日志系统。
- 小步集成：先替换手动控制和路线速度出口，再逐步补窗口失焦/页面切换等更多场景。

## 4. 模块接口草案

新增文件：

- `motioncommandarbiter.h`
- `motioncommandarbiter.cpp`
- `tests/test_motioncommandarbiter.cpp`

核心类型：

```cpp
class MotionCommandArbiter : public QObject
{
    Q_OBJECT

public:
    enum class Source { Manual, Route };
    enum class ManualInput { Forward, Backward, TurnLeft, TurnRight };

    struct ManualCommandConfig {
        double linearSpeed = 0.0;
        double angularSpeed = 0.0;
        bool buttonsEnabled = false;
        bool keysEnabled = false;
    };

    void setManualCommandConfig(const ManualCommandConfig &config);
    void setHeartbeatIntervalMs(int intervalMs);
    void setChassisConnected(bool connected);
    void setManualInputActive(ManualInput input, bool active, bool fromKeyboard);
    void setRouteCommand(double linear, double angular);
    void clearRouteCommand();
    void emergencyStop(const QString &reason);
    void stopAll(const QString &reason);

signals:
    void velocityCommand(double linear, double angular);
    void safetyStopRequested(const QString &reason);
};
```

## 5. 仲裁规则

1. 急停/stopAll 立即清空所有输入和路线命令，并发一次零速。
2. 底盘未连接时不发送非零速度；如果从已连接变为断开，发出安全停车信号供 `Home` 审计。
3. 路线命令优先级高于手动心跳；路线执行时由 `RouteFollower` 的 tick 驱动发送。
4. 手动输入允许组合成一个速度命令：
   - `Forward` 与 `Backward` 相互抵消。
   - `TurnLeft` 与 `TurnRight` 相互抵消。
   - 线速度和角速度可以同时存在，为后续斜向/边走边转留余地。
5. 手动输入释放后，如果仍有其它手动输入，立即发送新的合成速度；如果没有任何输入，立即发送零速。
6. 手动输入保持时，arbiter 用心跳定时器重复发送当前合成速度。

## 6. Home 集成点

- 构造函数中创建 `MotionCommandArbiter`。
- `RouteFollower::velocityCommand` 不再连接 `Home::sendVelocityCommand`，改连 arbiter 的 `setRouteCommand`。
- `MotionCommandArbiter::velocityCommand` 连接 `Home::sendVelocityCommand`。
- 按钮/键盘事件只更新 arbiter 输入状态，不直接调用 `sendVelocityCommand`。
- `handleImageSwitchToggled()` 更新按钮/键盘使能，并让 arbiter 重新计算命令。
- `handleStopButtonClicked()` 调用 `arbiter->emergencyStop("home_stop_button")`。
- `cancelRouteExecution()` 在取消 `RouteFollower` 后调用 `arbiter->clearRouteCommand()` 或 `stopAll()`。
- `handleChassisConnected()/handleChassisDisconnected()` 同步底盘连接状态。
- `handleNetworkFailure()` 将位姿置无效后，触发路线安全停车。
- `MainWindow::changeEvent()` 在窗口失活/最小化时调用 `Home::stopMotionForSafety()`。

## 7. 测试矩阵

| 场景 | 期望 |
|---|---|
| 按下前进 | 立即发正线速度 |
| 释放前进 | 立即发零速 |
| 前进+左转 | 发线速度+角速度 |
| 前进释放但左转保持 | 立即发纯角速度 |
| 左右同时按下 | 角速度抵消为 0 |
| 路线命令存在 | 发送路线命令，不被手动心跳覆盖 |
| 路线清除 | 如果无手动输入，发零速 |
| 急停 | 清空所有输入并发零速 |
| 底盘断连 | 非零命令不继续发送，并发安全停车信号 |
| 窗口失焦/最小化 | 取消路线、清空手动输入并发零速 |

## 8. 分阶段落地

### V1 本分支

- 新增 arbiter 与单测。
- Home 接入 arbiter，移除四个方向重复定时器的速度发送职责。
- MainWindow 接入失焦/最小化安全停车。
- 保持 UI 行为不变，优先修复释放不停车和多来源仲裁。

### V2 后续

- 页面切走、应用退出前的安全停车策略继续收敛到同一接口。
- 将手动控制审计集中到 arbiter 状态变化事件，减少重复日志代码。
- 增加假 `ChassisClient` 集成测试，覆盖 Home 到 WebSocket 出口。
- 结合数据库模块，把安全停车事件同步到 `operation_events`。

## 9. 面试表达要点

这部分可以表达为：把机器人运动控制从“事件驱动直接发命令”升级为“安全关键命令仲裁层”。它解决的不只是代码整洁，而是可靠性问题：

- 所有速度命令有唯一出口。
- 所有停车场景有统一兜底。
- 手动/自动来源有明确优先级。
- 核心规则脱离 UI 后可以单测。
