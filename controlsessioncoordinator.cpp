#include "controlsessioncoordinator.h"

ControlSessionCoordinator::ControlSessionCoordinator(TrackingClient *tracking, PoseClient *pose,
                                                     ChassisClient *chassis, MotionCommandArbiter *manual,
                                                     QObject *parent)
    : QObject(parent), m_tracking(tracking), m_pose(pose), m_chassis(chassis), m_manual(manual) {
    m_timer.setInterval(50);
    connect(&m_timer, &QTimer::timeout, this, &ControlSessionCoordinator::checkHandoff);
    m_resetTimer.setInterval(50);
    connect(&m_resetTimer, &QTimer::timeout, this, [this] {
        if (m_resetAge.elapsed() > 3000) {
            m_resetTimer.stop();
            emit message(tr("底盘解除锁存尚未确认，请检查后重新复位"));
            return;
        }
        if (!m_chassis || !m_tracking) {
            m_resetTimer.stop();
            return;
        }
        const auto state = m_chassis->controlState();
        if (state.stopped() && !state.estop && state.owner == "None" && m_tracking->fresh() &&
            m_tracking->snapshot().owner == "None" && m_tracking->snapshot().ownerEpoch == state.epoch) {
            m_resetTimer.stop();
            m_tracking->control("reset_fault");
        }
    });
    connect(manual, &MotionCommandArbiter::manualTakeoverRequested, this,
            &ControlSessionCoordinator::requestManual);
    connect(chassis, &ChassisClient::manualPermissionChanged, manual,
            &MotionCommandArbiter::setManualPermission);
    connect(chassis, &ChassisClient::manualPermissionChanged, this, [this](bool granted) {
        if (granted) {
            emit message(tr("底盘已确认手动接管"));
            if (m_tracking && !m_tracking->snapshot().executionId.isEmpty())
                m_tracking->control("manual_takeover");
        }
    });
    connect(pose, &PoseClient::frameChanged, this, [this] {
        cancelHandoff(tr("位姿服务或坐标版本已变化，请重新确认任务"));
        if (m_tracking && m_tracking->snapshot().isExecuting())
            m_tracking->control("pause");
    });
    connect(tracking, &TrackingClient::serverRestarted, this,
            [this] { cancelHandoff(tr("任务服务已重启，请重新上传任务")); });
    connect(tracking, &TrackingClient::sessionChanged, this, [this](bool ready) {
        if (!ready)
            cancelHandoff(tr("操作会话已失效，请重新获取操作权"));
    });
    connect(tracking, &TrackingClient::errorOccurred, this, &ControlSessionCoordinator::message);
    connect(pose, &PoseClient::errorOccurred, this, &ControlSessionCoordinator::message);
    connect(chassis, &ChassisClient::errorOccurred, this, &ControlSessionCoordinator::message);
    connect(tracking, &TrackingClient::commandUncertain, this, [this](const QString &op, const QString &) {
        cancelHandoff();
        emit message(tr("%1 结果未知，请核对工控机执行状态后再操作").arg(op));
    });
    connect(tracking, &TrackingClient::commandFinished, this,
            [this](const QString &op, const QJsonObject &result) {
                emit message(
                    tr("%1：%2 %3").arg(op, result["state"].toString(), result["reason"].toString()));
            });
}
void ControlSessionCoordinator::setBinding(const MapFrameBinding &b) {
    if (b.toJson() == m_binding.toJson())
        return;
    cancelHandoff();
    m_binding = b;
    if (m_tracking)
        m_tracking->invalidatePreparedPlan();
    if (m_tracking && m_tracking->snapshot().isExecuting())
        m_tracking->control("pause");
    emit bindingChanged(b);
}
bool ControlSessionCoordinator::coordinatesReady() const {
    if (!m_binding.isUsable() || !m_pose || !m_tracking || !m_pose->fresh() || !m_tracking->fresh())
        return false;
    const auto pose = m_pose->snapshot();
    const auto config = m_tracking->configuration();
    return pose.originRevision == m_binding.context.originRevision &&
           pose.calibrationId == m_binding.context.calibrationId &&
           config["origin"].toObject()["active"].toObject()["originRevision"].toString() ==
               m_binding.context.originRevision &&
           config["profile_revision"].toString() == m_binding.context.controllerProfileRevision;
}
void ControlSessionCoordinator::acquireSession() {
    if (m_tracking)
        m_tracking->acquireSession();
}
bool ControlSessionCoordinator::upload(const QJsonObject &plan) {
    cancelHandoff();
    if (!coordinatesReady()) {
        emit message(tr("地图与工控机原点、标定或控制参数版本未匹配"));
        return false;
    }
    if (!m_tracking->hasSession()) {
        emit message(tr("请先获取任务操作权，再上传计划"));
        return false;
    }
    return m_tracking->upload(plan);
}
void ControlSessionCoordinator::startTask() { beginMotion("start"); }
void ControlSessionCoordinator::resumeTask() { beginMotion("resume"); }
void ControlSessionCoordinator::beginMotion(const QString &operation) {
    if (m_resetTimer.isActive())
        return;
    if (!m_operation.isEmpty())
        return;
    if (!coordinatesReady() || !m_tracking->hasSession() || m_tracking->busy() || !m_chassis || !m_manual ||
        !m_chassis->controlFresh()) {
        emit message(tr("启动条件未满足：请检查坐标、会话、任务及底盘状态"));
        return;
    }
    const auto state = m_tracking->snapshot();
    const auto plan = m_tracking->preparedPlan();
    if (plan["taskId"].toString() != state.taskId || plan["revision"].toInt() != state.taskRevision ||
        plan["context"].toObject() != m_binding.context.toJson()) {
        emit message(tr("本地地图或计划已改变，请重新上传并确认任务"));
        return;
    }
    if ((operation == "start" && state.state != "Ready") ||
        (operation == "resume" && state.state != "Paused")) {
        emit message(tr("计划未就绪或任务不处于暂停状态"));
        return;
    }
    m_operation = operation;
    m_expectedBoot = state.bootId;
    m_expectedTask = state.taskId;
    m_expectedExecution = state.executionId;
    m_expectedStateVersion = state.stateVersion;
    m_manual->prepareAutomatic();
    emit manualInputsCleared();
    m_chassis->releaseManual();
    m_handoffAge.restart();
    m_stoppedAge.invalidate();
    m_timer.start();
    emit handoffChanged(true);
    emit message(tr("正在停止手动发送，等待底盘停稳和控制权释放"));
}
void ControlSessionCoordinator::cancelHandoff(const QString &reason) {
    if (m_operation.isEmpty())
        return;
    m_operation.clear();
    m_timer.stop();
    m_stoppedAge.invalidate();
    emit handoffChanged(false);
    if (!reason.isEmpty())
        emit message(reason);
}
void ControlSessionCoordinator::checkHandoff() {
    if (m_operation.isEmpty())
        return;
    if (!m_tracking || !m_chassis || !coordinatesReady() || !m_tracking->hasSession() ||
        m_handoffAge.elapsed() > 3000) {
        cancelHandoff(tr("自动交接失败或超时，保持停止，请重新操作"));
        return;
    }
    const auto status = m_tracking->snapshot();
    if (status.bootId != m_expectedBoot || status.taskId != m_expectedTask ||
        status.executionId != m_expectedExecution || status.stateVersion != m_expectedStateVersion) {
        cancelHandoff(tr("任务状态已变化，本次启动取消"));
        return;
    }
    const auto chassis = m_chassis->controlState();
    if (!chassis.stopped() || chassis.owner != "None" || chassis.estop || status.owner != "None" ||
        status.ownerEpoch != chassis.epoch || status.chassisBootId != chassis.bootId) {
        m_stoppedAge.invalidate();
        return;
    }
    if (!m_stoppedAge.isValid())
        m_stoppedAge.start();
    if (m_stoppedAge.elapsed() < 200 || !m_tracking->motionReady())
        return;
    const auto operation = m_operation;
    cancelHandoff();
    if (!m_tracking->control(operation))
        emit message(tr("启动许可已变化，请核对状态后重新操作"));
}
void ControlSessionCoordinator::pauseTask() {
    cancelHandoff();
    if (m_tracking)
        m_tracking->control("pause");
}
void ControlSessionCoordinator::abortTask() {
    cancelHandoff();
    if (m_tracking)
        m_tracking->control("abort");
}
void ControlSessionCoordinator::requestManual() {
    m_resetTimer.stop();
    cancelHandoff(tr("人工接管已取消自动启动"));
    if (m_chassis) {
        m_chassis->requestManual();
        emit message(tr("等待底盘确认手动接管"));
    }
}
void ControlSessionCoordinator::emergencyStop() {
    m_resetTimer.stop();
    cancelHandoff();
    if (m_manual)
        m_manual->prepareAutomatic();
    emit manualInputsCleared();
    if (m_chassis)
        m_chassis->stopLatched();
    if (m_tracking)
        m_tracking->control("estop");
    emit message(tr("已请求锁存停止，实际停止以底盘反馈为准"));
}
void ControlSessionCoordinator::resetFault() {
    cancelHandoff();
    if (!m_chassis || !m_tracking || !m_chassis->controlState().stopped() || !m_tracking->hasSession()) {
        emit message(tr("请等待底盘停稳并获取操作会话后复位"));
        return;
    }
    m_chassis->resetStopLatch();
    m_resetAge.start();
    m_resetTimer.start();
}
void ControlSessionCoordinator::shutdown() {
    m_resetTimer.stop();
    cancelHandoff();
    if (m_manual)
        m_manual->prepareAutomatic();
    if (m_chassis)
        m_chassis->releaseManual();
    // Keep the single safety write alive until event-loop teardown; no more session renewals.
    if (m_tracking) {
        m_tracking->control("pause");
        m_tracking->stopRenewingSession();
    }
    if (m_pose)
        m_pose->stop();
}
