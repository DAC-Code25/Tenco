#include "home_status_presenter.h"

#include "ui_mainwindow.h"
#include "trackingtypes.h"

#include <QJsonValue>
#include <QLineEdit>
#include <QMap>
#include <QObject>
#include <QStringList>
#include <QtMath>

#include <cmath>
#include <utility>

namespace {
constexpr const char *kConnectedStyle =
    "QPushButton { background:#e5f2eb; color:#246647; min-width:80px; min-height:38px;"
    " border:1px solid #c8dfd0; border-radius:7px; font-size:15px; font-family:'Microsoft YaHei'; }";
constexpr const char *kDisconnectedStyle =
    "QPushButton { background:#fff2df; color:#895313; min-width:80px; min-height:38px;"
    " border:1px solid #eddbbb; border-radius:7px; font-size:15px; font-family:'Microsoft YaHei'; }";

QString poseReason(const QString &code)
{
    static const QMap<QString, QString> reasons{
        {"nonfinite_estimate", QObject::tr("位姿数据无效")},
        {"initializing", QObject::tr("融合初始化中")},
        {"calibration_unverified", QObject::tr("车辆标定未完成")},
        {"uncertainty_unverified", QObject::tr("定位误差范围未确认")},
        {"uncertainty_unknown", QObject::tr("使用最终 HTTP 位姿，定位误差未评估")},
        {"braking_estimate", QObject::tr("使用保守制动估计，行驶速度由工控机导航配置与当前约束决定")},
        {"state_stale", QObject::tr("融合位姿已过期")},
        {"measured_twist_stale", QObject::tr("实测速度反馈已过期")},
        {"clock_jump", QObject::tr("位姿时钟异常")},
        {"transport_clock_mismatch", QObject::tr("位姿时钟不匹配")},
        {"position_observation_degraded", QObject::tr("位置观测已降级")},
        {"heading_observation_degraded", QObject::tr("航向观测已降级")},
        {"absolute_observation_times_unavailable", QObject::tr("绝对观测时间不可用")},
        {"source_observation_time_unavailable", QObject::tr("源观测时间未知，按实时 HTTP 接收更新")},
        {"pose_correction", QObject::tr("融合位姿正在修正")}};
    return reasons.value(code, QObject::tr("定位状态未确认"));
}

QString taskStateName(const QString &state)
{
    static const QMap<QString, QString> states{
        {"Idle", QObject::tr("待命")}, {"Ready", QObject::tr("任务就绪")},
        {"Starting", QObject::tr("正在启动")}, {"Aligning", QObject::tr("对准路径")},
        {"Tracking", QObject::tr("执行中")}, {"Braking", QObject::tr("减速停车")},
        {"Turning", QObject::tr("转向中")}, {"Waiting", QObject::tr("等待中")},
        {"Pausing", QObject::tr("正在暂停")}, {"Paused", QObject::tr("已暂停")},
        {"Aborting", QObject::tr("正在中止")}, {"Aborted", QObject::tr("已中止")},
        {"Completed", QObject::tr("已完成")}, {"Fault", QObject::tr("故障停车")}};
    return states.value(state, QObject::tr("状态未知"));
}

QString compactTaskReason(const QString &reason)
{
    static const QMap<QString, QString> reasons{
        {"operator_pause", QObject::tr("人工暂停")},
        {"operator_abort", QObject::tr("人工中止")},
        {"goal_position_outside_tolerance", QObject::tr("到点超差")},
        {"normal_handover_unstable", QObject::tr("位姿未稳定")},
        {"state_stale", QObject::tr("位姿更新超时")},
        {"pose_stale", QObject::tr("位姿更新延迟")},
        {"pose_hard_timeout", QObject::tr("定位更新中断")},
        {"pose_sample_invalid", QObject::tr("定位数据异常")},
        {"pose_sample_jump", QObject::tr("定位跳点")},
        {"pose_sample_unstable", QObject::tr("定位未稳定")},
        {"pose_invalid", QObject::tr("定位未就绪")},
        {"localization_invalid", QObject::tr("定位未就绪")},
        {"chassis_feedback_lost", QObject::tr("底盘反馈中断")},
        {"chassis_unavailable", QObject::tr("底盘未连接")},
        {"measured_twist_stale", QObject::tr("实测反馈超时")},
        {"control_cycle_late", QObject::tr("控制调度延迟")},
        {"lateral_safety_limit", QObject::tr("路径偏差过大")},
        {"turn_timeout", QObject::tr("转向超时")},
        {"turn_recovery_timeout", QObject::tr("转向回点超时")},
        {"turn_space_exceeded", QObject::tr("转向偏移过大")},
        {"external_action_timeout", QObject::tr("检查点等待超时")},
        {"external_action_failed", QObject::tr("检查点操作失败")},
        {"emergency_stop", QObject::tr("已触发急停")},
        {"command_expired", QObject::tr("控制指令过期")},
        {"progress_timeout", QObject::tr("前进超时")},
        {"terminal_stop_timeout", QObject::tr("停稳确认超时")},
        {"fusion_restarted", QObject::tr("定位服务重启")},
        {"device_restarted", QObject::tr("设备连接重启")},
        {"context_mismatch", QObject::tr("地图配置不匹配")}};
    return reasons.value(reason, QObject::tr("查看维护诊断"));
}

void setReadout(QLineEdit *field, const QString &text, const QString &detail)
{
    if (!field) return;
    field->setText(text);
    field->setToolTip(detail);
    field->setCursorPosition(0);
}
}

HomeStatusPresenter::HomeStatusPresenter(Ui::MainWindow *ui,
                                         LogHandler logHandler)
    : m_ui(ui)
    , m_logHandler(std::move(logHandler))
{
    setConnectionIndicator(false);
    if (m_ui) {
        for (auto *field : {m_ui->homeLocalizationState, m_ui->homeTaskState, m_ui->homeTaskProgress}) {
            field->setReadOnly(true);
            field->setFocusPolicy(Qt::NoFocus);
        }
        renderLocalizationStatus();
        setTaskStatus({}, false);
    }
}

void HomeStatusPresenter::handleStatusPacket(const QJsonObject &packet)
{
    const bool wasConnected = m_lastConnectionStatus;
    m_lastConnectionStatus = true;
    if (!wasConnected) {
        m_connectionRestored = true;
        if (m_logHandler) {
            m_logHandler(QObject::tr("底盘 HTTP 状态监视已恢复"));
        }
    }

    setConnectionIndicator(true);

    if (m_ui) {
        for (auto *field : {m_ui->lineEdit_BattryState, m_ui->lineEdit_BattryVol,
                           m_ui->lineEdit_BattryTemp, m_ui->lineEdit_RunTime})
            field->setToolTip(QObject::tr("来自底盘 HTTP 状态反馈。"));
    }

    const QJsonArray dataArray = packet.value(QStringLiteral("data")).toArray();
    for (const QJsonValue &item : dataArray) {
        const QJsonObject data = item.toObject();
        const QString addr = data.value(QStringLiteral("address")).toString();
        const QJsonArray values = data.value(QStringLiteral("value")).toArray();
        const QJsonValue firstValue = values.isEmpty() ? QJsonValue() : values.first();
        applyStatusField(StatusProtocol::fieldIdFromAddress(addr), values, firstValue);
    }
}

bool HomeStatusPresenter::handleNetworkFailure(int httpStatus, const QString &errorString, const QByteArray &responseBody)
{
    const bool wasConnected = m_lastConnectionStatus;
    m_lastConnectionStatus = false;
    m_connectionRestored = false;
    setConnectionIndicator(false);

    QString detail = QObject::tr("底盘 HTTP 状态监视延迟，正在重连；导航任务状态由工控机独立反馈");
    if (httpStatus > 0) {
        detail += QObject::tr(" (HTTP %1)").arg(httpStatus);
    }
    if (!errorString.isEmpty()) {
        detail += QObject::tr("：%1").arg(errorString);
    }
    if (m_logHandler) {
        m_logHandler(detail);
    }

    if (!responseBody.isEmpty() && m_logHandler) {
        const QString bodyPreview = QString::fromUtf8(responseBody.left(200)).trimmed();
        if (!bodyPreview.isEmpty()) {
            m_logHandler(QObject::tr("响应: %1").arg(bodyPreview));
        }
    }


    return wasConnected;
}

bool HomeStatusPresenter::takeConnectionRestored()
{
    if (!m_connectionRestored) {
        return false;
    }
    m_connectionRestored = false;
    return true;
}

void HomeStatusPresenter::clearConnectionRestored()
{
    m_connectionRestored = false;
}

void HomeStatusPresenter::showControlPose(QLineEdit *field, const ControlPoseSnapshot &pose)
{
    if (!field) return;
    if (pose.monitorDelayed) {
        field->setText(QObject::tr("位姿显示延迟，等待无线更新"));
        field->setToolTip(QObject::tr("保留上次位置；地图不会把旧位姿标成实时有效。导航使用工控机本地位姿独立执行。"));
        field->setCursorPosition(0);
        return;
    }
    if (pose.validForControl) {
        const auto coordinates = QObject::tr("X:%1, Y:%2, theta:%3")
            .arg(pose.position.x(), 0, 'f', 3).arg(pose.position.y(), 0, 'f', 3)
            .arg(pose.yaw, 0, 'f', 3);
        field->setText(coordinates);
        field->setToolTip(coordinates + QObject::tr("\n位置单位：米；角度单位：弧度\n位姿年龄：%1 毫秒")
            .arg(pose.stateAgeMs, 0, 'f', 0));
    } else {
        QStringList details;
        for (const auto &code : pose.reasons)
            details.append(QObject::tr("%1（%2）").arg(
                poseReason(code), code));
        const auto primary = pose.reasons.isEmpty() ? QObject::tr("定位状态未确认") :
            poseReason(pose.reasons.first());
        field->setText(primary);
        field->setToolTip(details.isEmpty() ? field->text() :
            QObject::tr("定位未就绪\n%1").arg(details.join('\n')));
    }
    field->setCursorPosition(0);
}

void HomeStatusPresenter::setControlPoseStatus(const ControlPoseSnapshot &pose)
{
    m_pose = pose;
    m_hasPose = !pose.bootId.isEmpty();
    m_poseMonitorCurrent = !pose.monitorDelayed;
    m_poseMonitorReason.clear();
    renderLocalizationStatus();
}

void HomeStatusPresenter::setPoseMonitoringAvailability(bool current, const QString &reason)
{
    m_poseMonitorCurrent = current;
    m_poseMonitorReason = reason;
    renderLocalizationStatus();
}

void HomeStatusPresenter::renderLocalizationStatus()
{
    if (!m_ui) return;
    QString text, detail;
    if (!m_hasPose) {
        text = QObject::tr("等待定位连接");
        detail = m_poseMonitorReason.isEmpty() ? QObject::tr("尚未收到工控机融合位姿。") : m_poseMonitorReason;
    } else if (!m_poseMonitorCurrent || m_pose.monitorDelayed) {
        text = QObject::tr("定位显示延迟");
        detail = QObject::tr("定位监视未更新，当前定位状态待确认。导航由工控机本地位姿独立执行。");
        if (!m_poseMonitorReason.isEmpty()) detail += '\n' + m_poseMonitorReason;
    } else {
        text = m_pose.validForControl ? QObject::tr("定位可用") :
            m_pose.reasons.contains("initializing") ? QObject::tr("融合初始化中") : QObject::tr("定位不可用");
        detail = m_pose.validForControl ? QObject::tr("工控机融合位姿可用于控制；不表示定位误差已经实测确认。") :
            QObject::tr("工控机融合位姿当前未就绪。");
        for (const auto &reason : m_pose.reasons) detail += '\n' + poseReason(reason);
        detail += QObject::tr("\n位姿年龄 %1 ms").arg(m_pose.stateAgeMs, 0, 'f', 0);
    }
    setReadout(m_ui->homeLocalizationState, text, detail);
}

void HomeStatusPresenter::setTaskStatus(const TrackingSnapshot &status, bool current)
{
    if (!m_ui) return;
    if (status.bootId.isEmpty() || status.state.isEmpty()) {
        setReadout(m_ui->homeTaskState, QObject::tr("自动任务：未连接"),
            QObject::tr("尚未收到工控机任务状态；不能据此判断车辆已停稳。"));
        setReadout(m_ui->homeTaskProgress, QObject::tr("等待任务状态"),
            QObject::tr("任务状态来自共享工控机导航监视，不额外轮询。"));
        return;
    }

    QString state = taskStateName(status.state);
    if (status.autoResumePending) state = QObject::tr("等待自动恢复");
    const auto reason = status.state == "Fault" ? status.faultCode : status.pauseReason;
    if (!reason.isEmpty()) state += QStringLiteral(" · ") + compactTaskReason(reason);

    QStringList progress;
    if (status.taskId.isEmpty()) {
        progress.append(QObject::tr("暂无任务"));
    } else if (status.isTerminal()) {
        progress.append(status.state == "Completed" ? QObject::tr("任务已完成") :
            status.state == "Aborted" ? QObject::tr("任务已中止") : QObject::tr("任务故障"));
    } else if (status.state == "Ready") {
        progress.append(QObject::tr("等待启动"));
    } else {
        progress.append(status.returningToStart ? QObject::tr("回起点准备") :
            QObject::tr("步骤 %1").arg(qint64(status.stepIndex) + 1));
        if (status.turnRecovering && std::isfinite(status.turnOffsetMeters)) {
            progress.append(QObject::tr("回点偏移 %1 m").arg(status.turnOffsetMeters, 0, 'f', 2));
        } else if ((status.state == "Turning" || status.state == "Aligning") && std::isfinite(status.remainingAngleRad)) {
            progress.append(QObject::tr("剩余 %1°").arg(qRadiansToDegrees(qAbs(status.remainingAngleRad)), 0, 'f', 1));
        } else if (status.state == "Waiting") {
            progress.append(!status.waitingEventId.isEmpty() ? QObject::tr("等待检查点确认") :
                QObject::tr("等待 %1 s").arg(qMax(0.0, status.remainingWaitMs) / 1000.0, 0, 'f', 1));
        } else if (std::isfinite(status.lengthMeters) && status.lengthMeters > 0 && std::isfinite(status.progressMeters)) {
            progress.append(QObject::tr("本段 %1/%2 m").arg(status.progressMeters, 0, 'f', 2)
                .arg(status.lengthMeters, 0, 'f', 2));
        }
        if (status.loopIndex > 0) progress.append(QObject::tr("第 %1 轮").arg(qint64(status.loopIndex) + 1));
    }

    QString detail = QObject::tr("工控机独立执行自动任务；此处仅展示状态，操作请进入地图页。");
    if (!status.taskId.isEmpty()) detail += QObject::tr("\n任务 %1\n步骤标识 %2").arg(status.taskId, status.stepId);
    if (!status.pauseReason.isEmpty()) detail += QObject::tr("\n暂停原因：%1").arg(TrackingJson::reasonText(status.pauseReason));
    if (!status.faultCode.isEmpty()) detail += QObject::tr("\n故障原因：%1").arg(TrackingJson::reasonText(status.faultCode));
    if (status.autoResumePending) detail += QObject::tr("\n恢复条件由工控机校验，通过后自动继续。");
    if (status.turnRecovering) detail += QObject::tr("\n转向回点中，偏移 %1 m").arg(status.turnOffsetMeters, 0, 'f', 2);
    if (!current) {
        const auto lastState = status.autoResumePending ? QObject::tr("等待恢复") : taskStateName(status.state);
        state = QObject::tr("显示延迟 · 上次%1").arg(lastState);
        progress.prepend(QObject::tr("上次"));
        detail = QObject::tr("无线任务监视未更新，显示上次状态；当前执行与停稳状态未知。\n") + detail;
    }
    setReadout(m_ui->homeTaskState, QObject::tr("自动任务：%1").arg(state), detail);
    setReadout(m_ui->homeTaskProgress, progress.join(QStringLiteral(" · ")),
        detail + QObject::tr("\n距离仅表示当前路径段进度，不是整个任务的完成比例。"));
}

void HomeStatusPresenter::setConnectionIndicator(bool connected)
{
    if (!m_ui || !m_ui->pushButton_10) {
        return;
    }
    if (connected) {
        m_ui->pushButton_10->setText(QObject::tr("底盘在线"));
        m_ui->pushButton_10->setStyleSheet(QString::fromUtf8(kConnectedStyle));
    } else {
        m_ui->pushButton_10->setText(QObject::tr("状态延迟"));
        m_ui->pushButton_10->setStyleSheet(QString::fromUtf8(kDisconnectedStyle));
        // Keep the original battery gauge and numeric speed visible. Connection
        // state and tooltips identify when their last values are not current.
        for (auto *field : {m_ui->lineEdit_BattryState, m_ui->lineEdit_BattryVol,
                           m_ui->lineEdit_BattryTemp, m_ui->lineEdit_RunTime}) {
            field->clear();
            field->setPlaceholderText(QObject::tr("待更新"));
            field->setToolTip(QObject::tr("底盘 HTTP 状态未更新，等待连接恢复。"));
        }
        m_ui->lcdNumber->setToolTip(QObject::tr("底盘状态未更新，当前数字为初始值或上次反馈，不能据此判断车辆已停稳。"));
        m_ui->battery->setToolTip(QObject::tr("底盘状态未更新，当前电量为初始显示或上次反馈。"));
    }
    m_ui->pushButton_10->setFocusPolicy(Qt::NoFocus);
    m_ui->pushButton_10->setCursor(Qt::ArrowCursor);
    m_ui->pushButton_10->setToolTip(QObject::tr("仅表示底盘 HTTP 状态监视连接。工控机导航与融合状态请查看维护界面；无线显示延迟不会暂停已受理的普通导航任务。"));
}

void HomeStatusPresenter::applyStatusField(StatusProtocol::FieldId fieldId,
                                           const QJsonArray &values,
                                           const QJsonValue &firstValue)
{
    if (!m_ui) {
        return;
    }

    switch (fieldId) {
    case StatusProtocol::FieldId::BatteryPercent: {
        const int battery = firstValue.toInt();
        if (m_ui->battery) {
            m_ui->battery->setValue(battery);
            m_ui->battery->setToolTip(QObject::tr("来自底盘电量反馈。"));
        }
        if (m_ui->lable_battray) {
            m_ui->lable_battray->setText(QString::number(battery) + "%");
        }
        break;
    }
    case StatusProtocol::FieldId::BatteryVoltage:
        if (m_ui->lineEdit_BattryVol) {
            m_ui->lineEdit_BattryVol->setText(QString::number(firstValue.toDouble(), 'f', 2) + "V");
        }
        break;
    case StatusProtocol::FieldId::WorkMode:
    case StatusProtocol::FieldId::MapName:
        // Legacy chassis metadata is independent of IPC task state.
        break;
    case StatusProtocol::FieldId::Velocity:
        if (m_ui->lcdNumber && !values.isEmpty()) {
            const double velocity = values.first().toDouble();
            if (velocity >= -2.0 && velocity <= 2.0) {
                m_ui->lcdNumber->setDigitCount(5);
                m_ui->lcdNumber->display(QString::number(qAbs(velocity), 'f', 2));
                m_ui->lcdNumber->setToolTip(QObject::tr("来自底盘速度反馈；自动任务停稳以工控机实测反馈与任务状态为准。"));
            }
        }
        break;
    case StatusProtocol::FieldId::BatteryTemperature:
        if (m_ui->lineEdit_BattryTemp) {
            m_ui->lineEdit_BattryTemp->setText(QString::number(firstValue.toInt()) + "℃");
        }
        break;
    case StatusProtocol::FieldId::ChargeDischargeState: {
        QString batteryState = QStringLiteral("电已充满");
        if (firstValue.toInt() == 1) {
            batteryState = QStringLiteral("充电中");
        } else if (firstValue.toInt() == 2) {
            batteryState = QStringLiteral("放电中");
        }
        if (m_ui->lineEdit_BattryState) {
            m_ui->lineEdit_BattryState->setText(batteryState);
        }
        break;
    }
    case StatusProtocol::FieldId::RunTime: {
        const int hour = values.size() > 0 ? values.at(0).toInt() : 0;
        const int minute = values.size() > 1 ? values.at(1).toInt() : 0;
        const int second = values.size() > 2 ? values.at(2).toInt() : 0;
        if (m_ui->lineEdit_RunTime) {
            m_ui->lineEdit_RunTime->setText(QStringLiteral("%1小时%2分钟%3秒").arg(hour).arg(minute).arg(second));
        }
        break;
    }
    case StatusProtocol::FieldId::Unknown:
        break;
    }
}
