#include "controlsessioncoordinator.h"

namespace {
QString operationText(const QString &operation) {
    if (operation == "prepare") return QObject::tr("下发任务");
    if (operation == "start") return QObject::tr("启动任务");
    if (operation == "resume") return QObject::tr("继续任务");
    if (operation == "pause") return QObject::tr("暂停任务");
    if (operation == "abort") return QObject::tr("结束任务");
    if (operation == "estop") return QObject::tr("紧急停止");
    if (operation == "reset_fault") return QObject::tr("故障复位");
    if (operation == "confirm_read") return QObject::tr("核对任务");
    if (operation == "origin") return QObject::tr("更新原点");
    return QObject::tr("操作");
}
QString operationStateText(const QString &state) {
    if (state == "applied") return QObject::tr("已应用");
    if (state == "accepted") return QObject::tr("已受理");
    if (state == "pending") return QObject::tr("等待确认");
    if (state == "failed" || state == "rejected") return QObject::tr("未执行");
    return QObject::tr("状态待确认");
}
}

ControlSessionCoordinator::ControlSessionCoordinator(TrackingClient *tracking, PoseClient *pose,
                                                     QObject *parent)
    : QObject(parent), m_tracking(tracking), m_pose(pose) {
    m_actionTimer.setInterval(50);
    connect(&m_actionTimer, &QTimer::timeout, this, &ControlSessionCoordinator::advanceAction);
    connect(tracking, &TrackingClient::availabilityChanged, this, [this](bool online, const QString &) {
        if (!online && (!m_tracking || !m_tracking->commandFresh()))
            cancelAction(tr("连接中断，待发送操作已取消；重连后请重新点击"));
    });
    connect(tracking, &TrackingClient::serverRestarted, this, [this] {
        cancelAction(tr("导航服务已重启，待发送操作已取消"));
        m_stopRequest.clear();
    });
    connect(tracking, &TrackingClient::statusChanged, this, &ControlSessionCoordinator::observeStop);
    connect(tracking, &TrackingClient::commandAccepted, this, [this](const QString &, const QString &id) {
        if (id == m_stopRequest)
            emit message(tr("工控机已受理停止请求，等待执行及实测停稳；请勿仅凭此提示判断车辆已停"));
    });
    connect(tracking, &TrackingClient::errorOccurred, this, [this](const QString &reason) {
        emit message(TrackingJson::reasonText(reason));
    });
    connect(pose, &PoseClient::errorOccurred, this, [this](const QString &reason) {
        emit message(TrackingJson::reasonText(reason));
    });
    connect(tracking, &TrackingClient::configurationChanged, this, [this](const QJsonObject &config) {
        if (m_pose)
            m_pose->setStateTimeoutMs(config["localizationPolicy"].toObject()["stateTimeoutMs"].toInt());
    });
    connect(tracking, &TrackingClient::commandUncertain, this, [this](const QString &op, const QString &id) {
        if (m_action == "deploy" && id == m_prepareRequestId)
            cancelAction(tr("任务下发结果未知，已取消自动启动；请核对工控机状态"));
        if (id == m_stopRequest) {
            m_stopRequest.clear();
            emit message(tr("停止请求确认超时，车辆是否停稳未知；不会自动重发，请核对现场并按需使用实体急停"));
        } else
            emit message(tr("%1结果未知，请核对工控机执行状态后再操作").arg(operationText(op)));
    });
    connect(tracking, &TrackingClient::commandFinished, this,
            [this](const QString &op, const QJsonObject &result) {
                if (!m_stopRequest.isEmpty() && result["requestId"].toString() == m_stopRequest) {
                    if (result["state"].toString() == "applied") {
                        m_stopApplied = true;
                        emit message(tr("工控机已应用停止请求，等待实测停稳确认"));
                        if (m_tracking) observeStop(m_tracking->snapshot());
                    } else {
                        m_stopRequest.clear();
                        emit message(tr("停止请求未执行：%1；尚未确认停车，请核对现场")
                            .arg(TrackingJson::reasonText(result["reason"].toString())));
                    }
                    return;
                }
                if (m_action == "deploy" && result["requestId"].toString() == m_prepareRequestId) {
                    if (result["state"].toString() != "applied")
                        cancelAction(tr("任务下发未完成，已取消自动启动：%1")
                                         .arg(TrackingJson::reasonText(result["reason"].toString())));
                    else {
                        m_prepareApplied = true;
                        emit message(tr("任务校验已完成，正在等待导航就绪"));
                        advanceAction();
                    }
                }
                if (op == "reset_fault" && result["state"].toString() == "applied") {
                    emit message(tr("故障已复位；旧任务已清除，请重新下发任务，车辆不会自动继续"));
                    return;
                }
                emit message(tr("%1：%2 %3").arg(operationText(op), operationStateText(result["state"].toString()),
                                               TrackingJson::reasonText(result["reason"].toString())));
            });
    connect(tracking, &TrackingClient::eventReceived, this, [this](const QJsonObject &event) {
        const auto reason = event["reason"].toString();
        if (reason == "path_alignment_braking" || reason == "path_realigning")
            emit message(TrackingJson::reasonText(reason));
    });
}
void ControlSessionCoordinator::setBinding(const MapFrameBinding &b) {
    if (b.toJson() == m_binding.toJson())
        return;
    cancelAction(tr("地图坐标已改变，待发送操作已取消"));
    m_binding = b;
    if (m_tracking)
        m_tracking->invalidatePreparedPlan();
    emit bindingChanged(b);
}
bool ControlSessionCoordinator::coordinatesReady() const {
    if (!m_binding.isUsable() || !m_pose || !m_tracking || !m_tracking->commandFresh())
        return false;
    const auto config = m_tracking->configuration();
    const bool fixedNavigation = config["navigation_defaults"].toObject()["rotation_policy"].toString() ==
                                 "vehicle_footprint";
    if (!fixedNavigation) {
        const auto pose = m_pose->snapshot();
        if (!m_pose->fresh() || pose.originRevision != m_binding.context.originRevision ||
            pose.calibrationId != m_binding.context.calibrationId)
            return false;
    }
    // Wireless display latency must not duplicate the IPC's local pose gate.
    return (!fixedNavigation || config["calibration"].toObject()["calibrationId"].toString() ==
                                   m_binding.context.calibrationId) &&
           config["origin"].toObject()["active"].toObject()["originRevision"].toString() ==
               m_binding.context.originRevision &&
           config["profile_revision"].toString() == m_binding.context.controllerProfileRevision;
}
void ControlSessionCoordinator::acquireSession() {
    if (m_tracking)
        m_tracking->acquireSession();
}
bool ControlSessionCoordinator::upload(const QJsonObject &plan) {
    if (!coordinatesReady()) {
        emit message(tr("地图与工控机原点、标定或控制参数版本未匹配"));
        return false;
    }
    return queueAction("prepare", plan);
}
bool ControlSessionCoordinator::uploadAndStart(const QJsonObject &plan) {
    if (!coordinatesReady() || plan["taskId"].toString().isEmpty() || plan["revision"].toInt() < 1 ||
        plan["context"].toObject() != m_binding.context.toJson()) {
        emit message(tr("无法下发并执行：地图与工控机坐标版本未匹配"));
        return false;
    }
    const auto status = m_tracking->snapshot();
    if (status.state != "Idle" && status.state != "Ready" && status.state != "Completed" &&
        status.state != "Aborted") {
        emit message(tr("请先结束当前任务或复位故障，再下发并执行新任务"));
        return false;
    }
    const bool accepted = queueAction("deploy", plan);
    if (!accepted) emit message(tr("当前已有操作正在处理，请等待完成后重新点击"));
    return accepted;
}
void ControlSessionCoordinator::cancelPendingAction() {
    cancelAction(tr("已取消后续启动；已受理的任务可在核对后手动启动"));
}
void ControlSessionCoordinator::startTask() { beginMotion("start"); }
bool ControlSessionCoordinator::confirmReadTask(const QJsonObject &record) {
    if (!coordinatesReady() || !queueAction("confirm_read", record)) {
        emit message(tr("接续确认未完成：请核对地图版本、定位，并重新读取当前任务"));
        return false;
    }
    return true;
}
void ControlSessionCoordinator::resumeTask() { beginMotion("resume"); }
bool ControlSessionCoordinator::updateOrigin(const QJsonObject &origin, const QString &expectedRevision) {
    return queueAction("origin", {{"origin", origin}, {"expectedRevision", expectedRevision}});
}
void ControlSessionCoordinator::beginMotion(const QString &operation) {
    if (!coordinatesReady() || m_tracking->busy()) {
        emit message(tr("启动条件未满足：请检查坐标及任务状态"));
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
    if (!queueAction(operation))
        emit message(tr("启动请求暂未就绪，请核对任务状态后重试"));
}
bool ControlSessionCoordinator::queueAction(const QString &operation, const QJsonObject &payload) {
    if (!m_tracking || !m_tracking->commandFresh() || !m_action.isEmpty())
        return false;
    if ((operation == "prepare" || operation == "deploy" || operation == "confirm_read" || operation == "start" || operation == "resume") &&
        m_tracking->busy())
        return false;
    const auto status = m_tracking->snapshot();
    m_action = operation;
    m_actionPayload = payload;
    m_actionBoot = status.bootId;
    m_actionTask = status.taskId;
    m_actionExecution = status.executionId;
    m_actionState = status.state;
    m_actionRevision = status.taskRevision;
    m_actionVersion = status.stateVersion;
    m_prepareRequestId.clear();
    m_prepareApplied = m_readyObserved = false;
    m_readyVersion = 0;
    m_sessionAttempts = 0;
    m_nextSessionAttempt = 0;
    m_actionAge.start();
    m_actionTimer.start();
    if (operation == "prepare" || operation == "deploy" || operation == "confirm_read" ||
        operation == "start" || operation == "resume")
        m_tracking->refreshConfiguration();
    emit actionPendingChanged(true);
    advanceAction();
    return true;
}
void ControlSessionCoordinator::cancelAction(const QString &reason) {
    const bool pending = !m_action.isEmpty();
    m_actionTimer.stop();
    m_action.clear();
    m_actionPayload = {};
    m_prepareRequestId.clear();
    if (pending) emit actionPendingChanged(false);
    if (pending && !reason.isEmpty()) emit message(reason);
}
void ControlSessionCoordinator::advanceAction() {
    if (m_action.isEmpty()) return;
    if (!m_tracking || !m_tracking->commandFresh()) {
        cancelAction(tr("连接中断，待发送操作已取消；重连后请重新点击"));
        return;
    }
    const auto status = m_tracking->snapshot();
    if (m_action == "deploy" && !m_prepareRequestId.isEmpty()) {
        const bool original = status.taskId == m_actionTask && status.taskRevision == m_actionRevision &&
            status.executionId == m_actionExecution && status.state == m_actionState &&
            status.stateVersion == m_actionVersion;
        const bool ready = status.taskId == m_actionPayload["taskId"].toString() &&
            status.taskRevision == m_actionPayload["revision"].toInt() && status.state == "Ready" &&
            status.executionId.isEmpty() && status.stateVersion > m_actionVersion;
        if (status.bootId != m_actionBoot || (!original && !ready) ||
            (m_readyObserved && (!ready || status.stateVersion != m_readyVersion))) {
            cancelAction(tr("任务身份或状态已改变，已取消自动启动；请核对后重新操作"));
            return;
        }
        if (m_actionAge.elapsed() > 15000) {
            cancelAction(tr("等待导航就绪超时，已取消自动启动；请核对后重新操作"));
            return;
        }
        if (!m_tracking->configurationFresh()) return;
        if (!coordinatesReady() || m_tracking->preparedPlan() != m_actionPayload) {
            cancelAction(tr("地图坐标或准备计划已改变，已取消自动启动"));
            return;
        }
        if (ready && !m_readyObserved) {
            m_readyObserved = true;
            m_readyVersion = status.stateVersion;
        }
        if (!m_prepareApplied || !ready || !m_tracking->motionReady()) return;
        // The same explicit click authorizes exactly one start after this prepare.
        // TrackingClient resolves its requestId by queries, never by resending motion.
        cancelAction();
        if (m_tracking->control("start")) emit message(tr("任务已就绪，已发送启动请求"));
        else emit message(tr("任务已就绪但启动未发送，请核对后手动启动"));
        return;
    }
    if (status.bootId != m_actionBoot || status.taskId != m_actionTask ||
        status.taskRevision != m_actionRevision || status.stateVersion != m_actionVersion ||
        status.executionId != m_actionExecution || status.state != m_actionState) {
        cancelAction(tr("任务状态已改变，待发送操作已取消，请核对后重新操作"));
        return;
    }
    if (m_actionAge.elapsed() > 10000) {
        cancelAction(tr("任务操作准备超时，未发送操作；请检查连接或其他操作端后重试"));
        return;
    }
    if (!m_tracking->hasSession()) {
        if (m_sessionAttempts < 2 && m_actionAge.elapsed() >= m_nextSessionAttempt &&
            m_tracking->acquireSession()) {
            ++m_sessionAttempts;
            m_nextSessionAttempt = m_actionAge.elapsed() + 1500;
        }
        return;
    }
    const bool motion = m_action == "start" || m_action == "resume";
    const bool needsCoordinates = motion || m_action == "prepare" || m_action == "deploy" || m_action == "confirm_read";
    if (needsCoordinates && !m_tracking->configurationFresh()) return;
    if (needsCoordinates && !coordinatesReady()) {
        cancelAction(tr("地图与工控机坐标版本不一致，操作已取消"));
        return;
    }
    if (motion && !m_tracking->motionReady()) return;
    if (m_action == "reset_fault" && !m_tracking->canResetFault()) {
        cancelAction(tr("复位条件已改变，操作未发送；请确认故障状态及底盘实测停稳后重新点击"));
        return;
    }
    if (m_action == "deploy") {
        if (!m_tracking->upload(m_actionPayload, &m_prepareRequestId)) {
            cancelAction(tr("任务未发送，已取消自动启动；请核对当前状态后重试"));
            return;
        }
        emit message(tr("正在下发任务；校验完成且就绪后将自动执行，可随时取消后续启动"));
        return;
    }
    const auto operation = m_action;
    const auto payload = m_actionPayload;
    cancelAction(); // A sent request is resolved by requestId; never resubmit it.
    const bool sent = operation == "prepare" ? m_tracking->upload(payload)
        : operation == "confirm_read" ? m_tracking->confirmReadTask(payload, m_binding.context)
        : operation == "origin" ? m_tracking->updateOrigin(payload["origin"].toObject(),
                                                          payload["expectedRevision"].toString())
        : m_tracking->control(operation);
    if (!sent) emit message(tr("操作未发送，请核对当前任务状态后重试"));
    else if (operation == "confirm_read")
        emit message(tr("已确认工控机当前计划；请明确点击启动或继续"));
}
void ControlSessionCoordinator::pauseTask() {
    requestStop("pause");
}
void ControlSessionCoordinator::abortTask() {
    requestStop("abort");
}
void ControlSessionCoordinator::emergencyStop() {
    requestStop("estop");
}
void ControlSessionCoordinator::requestStop(const QString &operation) {
    cancelAction();
    QString id;
    if (!m_tracking || !m_tracking->control(operation, &id)) {
        emit message(tr("停止请求未发送：连接未配置、任务身份未知或同类停止请求尚待确认；尚未确认停车，请核对现场，必要时使用实体急停"));
        return;
    }
    const auto status = m_tracking->snapshot();
    m_stopRequest = id;
    m_stopOperation = operation;
    m_stopBoot = status.bootId;
    m_stopExecution = status.executionId;
    m_stopSeq = status.seq;
    m_stopApplied = false;
    emit message(tr("停止请求已发送，等待工控机确认；尚未确认车辆停稳"));
}
void ControlSessionCoordinator::observeStop(const TrackingSnapshot &status) {
    if (m_stopRequest.isEmpty() || !m_stopApplied || !m_tracking || !m_tracking->fresh() ||
        status.seq <= m_stopSeq)
        return;
    if (status.bootId != m_stopBoot || status.executionId != m_stopExecution) {
        m_stopRequest.clear();
        emit message(tr("工控机或任务身份已改变，原停止请求的停车状态无法确认，请核对当前现场"));
        return;
    }
    const bool stoppedState = status.state == "Fault" || status.state == "Aborted" ||
        status.state == "Completed" || (m_stopOperation == "pause" && status.state == "Paused");
    if (stoppedState && status.chassisConnected && status.measuredValid && status.measuredStopped) {
        m_stopRequest.clear();
        emit message(tr("工控机已确认本次停止请求完成，底盘实测已停稳"));
    }
}
void ControlSessionCoordinator::resetFault() {
    cancelAction();
    if (!m_tracking || !m_tracking->commandFresh()) {
        emit message(tr("复位未发送：请先连接工控机并获取当前导航状态"));
    } else if (m_tracking->snapshot().state != "Fault") {
        emit message(tr("当前导航没有需要复位的故障；暂停任务请使用继续或结束"));
    } else if (m_tracking->busy()) {
        emit message(tr("复位未发送：仍有操作结果待确认，请等待工控机确认后重试"));
    } else if (!m_tracking->canResetFault()) {
        emit message(tr("复位未发送：请确认底盘在线、反馈有效且实测已停稳"));
    } else if (!queueAction("reset_fault")) {
        emit message(tr("复位未发送：请核对当前故障及连接状态后重试"));
    }
}
void ControlSessionCoordinator::shutdown() {
    cancelAction();
    // Closing the planner must not pause an accepted IPC task.
    if (m_tracking)
        m_tracking->stop();
    if (m_pose)
        m_pose->stop();
}
