#include "controlsessioncoordinator.h"

ControlSessionCoordinator::ControlSessionCoordinator(TrackingClient *tracking, PoseClient *pose,
                                                     QObject *parent)
    : QObject(parent), m_tracking(tracking), m_pose(pose) {
    connect(tracking, &TrackingClient::errorOccurred, this, &ControlSessionCoordinator::message);
    connect(pose, &PoseClient::errorOccurred, this, &ControlSessionCoordinator::message);
    connect(tracking, &TrackingClient::configurationChanged, this, [this](const QJsonObject &config) {
        if (m_pose)
            m_pose->setStateTimeoutMs(config["localizationPolicy"].toObject()["stateTimeoutMs"].toInt());
    });
    connect(tracking, &TrackingClient::commandUncertain, this, [this](const QString &op, const QString &) {
        emit message(tr("%1 结果未知，请核对工控机执行状态后再操作").arg(op));
    });
    connect(tracking, &TrackingClient::commandFinished, this,
            [this](const QString &op, const QJsonObject &result) {
                emit message(tr("%1：%2 %3").arg(op, result["state"].toString(), result["reason"].toString()));
            });
}
void ControlSessionCoordinator::setBinding(const MapFrameBinding &b) {
    if (b.toJson() == m_binding.toJson())
        return;
    m_binding = b;
    if (m_tracking)
        m_tracking->invalidatePreparedPlan();
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
bool ControlSessionCoordinator::confirmReadTask(const QJsonObject &record) {
    if (!coordinatesReady() || !m_tracking->confirmReadTask(record, m_binding.context)) {
        emit message(tr("接续确认未完成：请核对地图版本、定位、操作权，并重新读取当前任务"));
        return false;
    }
    emit message(tr("已确认工控机当前计划；任务不会自动启动，请明确点击启动或继续"));
    return true;
}
void ControlSessionCoordinator::resumeTask() { beginMotion("resume"); }
void ControlSessionCoordinator::beginMotion(const QString &operation) {
    if (!coordinatesReady() || !m_tracking->hasSession() || m_tracking->busy()) {
        emit message(tr("启动条件未满足：请检查坐标、操作会话及任务状态"));
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
    if (!m_tracking->control(operation))
        emit message(tr("启动请求暂未就绪，请核对任务状态后重试"));
}
void ControlSessionCoordinator::pauseTask() {
    if (m_tracking)
        m_tracking->control("pause");
}
void ControlSessionCoordinator::abortTask() {
    if (m_tracking)
        m_tracking->control("abort");
}
void ControlSessionCoordinator::emergencyStop() {
    if (m_tracking)
        m_tracking->control("estop");
    emit message(tr("已请求停止自动任务"));
}
void ControlSessionCoordinator::resetFault() {
    if (m_tracking)
        m_tracking->control("reset_fault");
}
void ControlSessionCoordinator::shutdown() {
    // Closing the planner must not pause an accepted IPC task.
    if (m_tracking)
        m_tracking->stop();
    if (m_pose)
        m_pose->stop();
}
