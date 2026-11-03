#include "navigationdiagnosticspanel.h"
#include "trackingclient.h"
#include "trackingtypes.h"

#include <QDateTime>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QProgressBar>
#include <QResizeEvent>
#include <QShowEvent>
#include <QVBoxLayout>
#include <cmath>

namespace {
constexpr int PollIntervalMs = 2000;
constexpr int SnapshotLifetimeMs = 5000;

struct StatusColors {
    const char *text;
    const char *light;
    const char *halo;
};
StatusColors statusColors(const QString &tone) {
    if (tone == "ok")
        return {"#216b46", "#2f9b67", "#e3f3e9"};
    if (tone == "error")
        return {"#b02d2d", "#d65151", "#fbe8e8"};
    if (tone == "warning")
        return {"#956000", "#d99a29", "#fbf0d9"};
    return {"#526259", "#97a49d", "#edf1ee"};
}
void colorLight(QFrame *light, const QString &tone) {
    const auto colors = statusColors(tone);
    light->setProperty("statusTone", tone);
    light->setStyleSheet(QStringLiteral("background-color:%1; border:4px solid %2; border-radius:9px;")
        .arg(colors.light, colors.halo));
}
QFrame *makeLight(QWidget *parent) {
    auto *light = new QFrame(parent);
    light->setFixedSize(18, 18);
    light->setFrameShape(QFrame::NoFrame);
    colorLight(light, {});
    return light;
}

bool nonnegative(const QJsonValue &value) {
    return value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() >= 0;
}
QString percentage(const QJsonValue &value) {
    if (!nonnegative(value) || value.toDouble() > 100)
        return QStringLiteral("N/A");
    return QString::number(value.toDouble(), 'f', 1) + QStringLiteral(" %");
}
QString availableCapacity(const QJsonObject &object) {
    const auto available = object.value("availableBytes"), total = object.value("totalBytes");
    if (!nonnegative(available) || !nonnegative(total) || total.toDouble() <= 0 ||
        available.toDouble() > total.toDouble())
        return QObject::tr("可用容量 N/A");
    constexpr double GiB = 1024.0 * 1024 * 1024;
    return QObject::tr("可用 %1 / %2 GiB")
        .arg(available.toDouble() / GiB, 0, 'f', 2).arg(total.toDouble() / GiB, 0, 'f', 2);
}
QString fixMode(const QJsonValue &value) {
    static const QHash<QString, QString> names{{"invalid", QObject::tr("无定位")},
        {"single", QObject::tr("单点")}, {"differential", QObject::tr("差分")},
        {"rtk_fixed", QObject::tr("RTK 固定解")}, {"rtk_float", QObject::tr("RTK 浮点解")}};
    return names.value(value.toString(), QStringLiteral("N/A"));
}
QString stateName(const QString &state) {
    static const QHash<QString, QString> names{{"Idle", QObject::tr("空闲")}, {"Ready", QObject::tr("待启动")},
        {"Starting", QObject::tr("启动中")}, {"Aligning", QObject::tr("对准中")},
        {"Tracking", QObject::tr("跟踪中")}, {"Braking", QObject::tr("制动中")},
        {"Turning", QObject::tr("转向中")}, {"Waiting", QObject::tr("等待事件")},
        {"Pausing", QObject::tr("暂停停车中")}, {"Paused", QObject::tr("已暂停")},
        {"Aborting", QObject::tr("中止中")}, {"Aborted", QObject::tr("已中止")},
        {"Completed", QObject::tr("已完成")}, {"Fault", QObject::tr("故障")}};
    return names.value(state);
}
QString sensorText(const QJsonObject &sensor) {
    const auto status = sensor.value("status").toString();
    QString text = status == "online" ? QObject::tr("在线") :
                   status == "offline" ? QObject::tr("离线") : QObject::tr("未知");
    if (sensor.value("valid").isBool())
        text += sensor.value("valid").toBool() ? QObject::tr(" · 数据有效") : QObject::tr(" · 数据无效");
    if (nonnegative(sensor.value("ageMs")))
        text += QObject::tr(" · 数据年龄 %1 ms").arg(sensor.value("ageMs").toDouble(), 0, 'f', 0);
    return text;
}
QString sensorState(const QJsonObject &sensor) {
    const auto status = sensor.value("status").toString();
    return status == "offline" ? QObject::tr("离线") : status == "online" ?
        (sensor.value("valid").toBool() ? QObject::tr("在线") : QObject::tr("数据异常")) : QObject::tr("未知");
}
QString sensorAge(const QJsonObject &sensor) {
    return nonnegative(sensor.value("ageMs")) ?
        QObject::tr("数据年龄 %1 ms").arg(sensor.value("ageMs").toDouble(), 0, 'f', 0) : QObject::tr("尚未收到数据");
}
QString sensorTone(const QJsonObject &sensor) {
    if (sensor.value("status") == "offline")
        return "error";
    if (sensor.value("status") == "online")
        return sensor.value("valid").toBool() ? "ok" : "error";
    return {};
}
} // namespace

NavigationDiagnosticsPanel::NavigationDiagnosticsPanel(QWidget *parent)
    : QWidget(parent), m_poseHttp(this), m_trackingHttp(this), m_resourcesHttp(this), m_timer(this) {
    setObjectName("navigationDiagnosticsPanel");
    setStyleSheet(R"(
QWidget#navigationDiagnosticsPanel { background: transparent; }
QLabel { font-family: "Microsoft YaHei"; }
QFrame#navigationSummaryCard, QFrame#navigationSensorCard, QFrame#navigationResourcesCard {
    background: #ffffff; border: 1px solid #dce5df; border-radius: 14px;
}
QFrame#navigationTaskStrip { background: #eaf0eb; border: none; border-radius: 10px; }
QLabel[diagnosticRole="title"] { color:#20392b; font-size:24px; font-weight:700; }
QLabel[diagnosticRole="heading"] { color:#294333; font-size:15px; font-weight:700; }
QLabel[diagnosticRole="caption"] { color:#738278; font-size:12px; }
QLabel[diagnosticRole="detail"] { color:#4e6355; font-size:13px; }
QLabel[diagnosticRole="state"] { font-size:13px; font-weight:600; }
QLabel[diagnosticRole="metric"] { font-size:21px; font-weight:700; }
QProgressBar { background:#edf2ee; border:none; border-radius:3px; }
QProgressBar::chunk { background:#7aa58a; border-radius:3px; }
)");
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(14);
    auto textLabel = [this](const QString &text, const char *role) {
        auto *label = new QLabel(text, this);
        label->setProperty("diagnosticRole", role);
        label->setWordWrap(true);
        label->setMinimumWidth(0);
        return label;
    };
    auto *heading = new QHBoxLayout;
    auto *titles = new QVBoxLayout;
    titles->setSpacing(4);
    titles->addWidget(textLabel(tr("导航状态"), "title"));
    titles->addWidget(textLabel(tr("传感器、导航节点与工控机资源"), "caption"));
    heading->addLayout(titles, 1);
    auto *refresh = textLabel(tr("每 2 秒刷新"), "caption");
    refresh->setToolTip(tr("离开页面停止查询。在线仅表示数据新鲜，是否可导航由控制系统独立判断。"));
    heading->addWidget(refresh);
    layout->addLayout(heading);
    m_summaryGrid = new QGridLayout;
    m_summaryGrid->setSpacing(12);
    m_summaryCards = {createStatusCard("service", tr("传感器诊断"), false),
                      createStatusCard("fusion", tr("融合输出"), false),
                      createStatusCard("tracking", tr("导航节点"), false)};
    layout->addLayout(m_summaryGrid);
    auto *task = new QFrame(this);
    task->setObjectName("navigationTaskStrip");
    auto *taskRow = new QHBoxLayout(task);
    taskRow->setContentsMargins(14, 10, 14, 10);
    taskRow->setSpacing(16);
    taskRow->addWidget(textLabel(tr("当前任务"), "caption"));
    auto *taskValue = textLabel({}, "detail");
    taskValue->setObjectName("navigationDiagnostics_task");
    taskValue->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_values.insert("task", taskValue);
    taskRow->addWidget(taskValue, 1);
    layout->addWidget(task);
    m_bodyGrid = new QGridLayout;
    m_bodyGrid->setSpacing(14);
    m_sensorSection = new QWidget(this);
    auto *sensorBox = new QVBoxLayout(m_sensorSection);
    sensorBox->setContentsMargins(0, 0, 0, 0);
    sensorBox->setSpacing(10);
    auto *sensorHeading = new QHBoxLayout;
    sensorHeading->addWidget(textLabel(tr("传感器"), "heading"));
    sensorHeading->addStretch();
    for (const auto &item : {qMakePair(QStringLiteral("ok"), tr("正常")),
                            qMakePair(QStringLiteral("error"), tr("异常")),
                            qMakePair(QString(), tr("未知"))}) {
        auto *entry = new QHBoxLayout;
        entry->setSpacing(3);
        auto *light = makeLight(this);
        light->setAccessibleName(item.second);
        colorLight(light, item.first);
        entry->addWidget(light);
        entry->addWidget(textLabel(item.second, "caption"));
        sensorHeading->addLayout(entry);
    }
    sensorBox->addLayout(sensorHeading);
    m_sensorGrid = new QGridLayout;
    m_sensorGrid->setSpacing(12);
    m_sensorCards = {createStatusCard("encoder", tr("编码器"), true),
                     createStatusCard("frontRtk", tr("前 RTK"), true),
                     createStatusCard("rearRtk", tr("后 RTK"), true),
                     createStatusCard("imu", tr("IMU"), true)};
    sensorBox->addLayout(m_sensorGrid, 1);
    auto *resources = new QFrame(this);
    resources->setObjectName("navigationResourcesCard");
    m_resourceSection = resources;
    auto *resourceBox = new QVBoxLayout(resources);
    resourceBox->setContentsMargins(18, 16, 18, 16);
    resourceBox->setSpacing(8);
    resourceBox->addWidget(textLabel(tr("工控机资源"), "heading"));
    m_resourcesState = textLabel({}, "caption");
    m_resourcesState->setObjectName("navigationResourcesState");
    resourceBox->addWidget(m_resourcesState);
    for (const auto &item : {qMakePair(QStringLiteral("cpu"), tr("CPU")),
                            qMakePair(QStringLiteral("gpu"), tr("GPU")),
                            qMakePair(QStringLiteral("memory"), tr("内存")),
                            qMakePair(QStringLiteral("disk"), tr("磁盘"))}) {
        auto *row = new QHBoxLayout;
        row->addWidget(textLabel(item.second, "caption"));
        auto *value = textLabel({}, "metric");
        value->setObjectName("navigationDiagnostics_" + item.first);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_values.insert(item.first, value);
        row->addWidget(value, 1, Qt::AlignRight);
        resourceBox->addLayout(row);
        auto *bar = new QProgressBar(resources);
        bar->setRange(0, 1000);
        bar->setTextVisible(false);
        bar->setFixedHeight(5);
        bar->setAccessibleName(tr("%1使用率").arg(item.second));
        m_bars.insert(item.first, bar);
        resourceBox->addWidget(bar);
        auto *detail = textLabel({}, "caption");
        detail->setObjectName("navigationDiagnosticsDetail_" + item.first);
        m_details.insert(item.first, detail);
        resourceBox->addWidget(detail);
    }
    layout->addLayout(m_bodyGrid);
    auto *foot = new QHBoxLayout;
    foot->setSpacing(18);
    for (const auto &item : {qMakePair(QStringLiteral("uptime"), tr("运行时间")),
                            qMakePair(QStringLiteral("sampledAt"), tr("更新于"))}) {
        foot->addWidget(textLabel(item.second, "caption"));
        auto *value = textLabel({}, "caption");
        value->setObjectName("navigationDiagnostics_" + item.first);
        m_values.insert(item.first, value);
        foot->addWidget(value);
    }
    foot->addStretch();
    layout->addLayout(foot);
    layout->addStretch();
    arrangeCards();
    m_timer.setInterval(PollIntervalMs);
    connect(&m_timer, &QTimer::timeout, this, &NavigationDiagnosticsPanel::poll);
    clearDiagnostics(tr("尚未连接"));
    clearResources(tr("尚未连接"));
    clearTracking(tr("尚未连接"));
}
NavigationDiagnosticsPanel::~NavigationDiagnosticsPanel() {
    m_timer.stop();
    m_poseHttp.cancelAll();
    m_trackingHttp.cancelAll();
    m_resourcesHttp.cancelAll();
}
void NavigationDiagnosticsPanel::setTrackingClient(TrackingClient *client) {
    m_trackingHttp.cancelAll();
    m_trackingClient = client;
    clearTracking(tr("等待导航状态"));
    if (isVisible()) poll();
}
void NavigationDiagnosticsPanel::readSharedTrackingStatus() {
    if (!m_trackingClient->matchesConfiguration(m_connection.trackingUrl, m_connection.token, m_connection.trackingTimeoutMs) ||
        !m_trackingClient->connected()) {
        m_status = {};
        m_statusAge.invalidate();
        setValue("task", tr("导航状态已过期"), "warning");
        m_values.value("task")->setToolTip({});
        return;
    }
    const auto status = m_trackingClient->snapshot();
    m_status = {{"bootId", status.bootId}, {"state", status.state}, {"autoResumePending", status.autoResumePending},
        {"pauseReason", status.pauseReason}, {"faultCode", status.faultCode}, {"monitorDelayed", !m_trackingClient->fresh()}};
    m_statusAge.restart();
    renderTracking();
}
bool NavigationDiagnosticsPanel::readSharedTrackingHealth() {
    if (!m_trackingClient ||
        !m_trackingClient->matchesConfiguration(m_connection.trackingUrl, m_connection.token, m_connection.trackingTimeoutMs) ||
        !m_trackingClient->healthFresh()) return false;
    m_health = m_trackingClient->health();
    m_healthReceivedAgeMs = m_trackingClient->healthAgeMs();
    m_healthAge.restart();
    return true;
}
QFrame *NavigationDiagnosticsPanel::createStatusCard(const QString &key, const QString &title, bool sensor) {
    auto *card = new QFrame(this);
    card->setObjectName(sensor ? "navigationSensorCard" : "navigationSummaryCard");
    card->setMinimumWidth(0);
    auto *box = new QVBoxLayout(card);
    box->setContentsMargins(16, 14, 16, 14);
    box->setSpacing(10);
    auto *name = new QLabel(title, card);
    name->setProperty("diagnosticRole", sensor ? "heading" : "caption");
    auto *heading = new QHBoxLayout;
    heading->setSpacing(12);
    heading->addWidget(name, 1);
    auto *badge = new QFrame(card);
    badge->setObjectName("navigationStatusBadge");
    auto *row = new QHBoxLayout(badge);
    row->setContentsMargins(5, 3, 9, 3);
    row->setSpacing(4);
    auto *light = makeLight(badge);
    light->setObjectName("navigationDiagnosticsLight_" + key);
    light->setAccessibleName(tr("%1状态灯").arg(title));
    auto *label = new QLabel(badge);
    label->setObjectName("navigationDiagnostics_" + key);
    label->setProperty("diagnosticRole", "state");
    label->setMinimumWidth(0);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    row->addWidget(light, 0, Qt::AlignTop);
    row->addWidget(label, 1);
    heading->addWidget(badge);
    box->addLayout(heading);
    auto *detail = new QLabel(card);
    detail->setObjectName("navigationDiagnosticsDetail_" + key);
    detail->setProperty("diagnosticRole", "detail");
    detail->setWordWrap(true);
    detail->setMinimumWidth(0);
    detail->setTextInteractionFlags(Qt::TextSelectableByMouse);
    box->addWidget(detail);
    m_details.insert(key, detail);
    if (sensor) {
        auto *note = new QLabel(card);
        note->setObjectName("navigationDiagnosticsNote_" + key);
        note->setProperty("diagnosticRole", "caption");
        note->setWordWrap(true);
        box->addWidget(note);
        m_notes.insert(key, note);
    }
    m_lights.insert(key, light);
    m_badges.insert(key, badge);
    m_values.insert(key, label);
    return card;
}
void NavigationDiagnosticsPanel::setValue(const QString &key, const QString &text, const QString &tone) {
    auto *label = m_values.value(key);
    label->setText(text);
    label->setToolTip(text);
    label->setProperty("statusTone", tone);
    label->setStyleSheet(QStringLiteral("color:%1;").arg(statusColors(tone).text));
    if (auto *light = m_lights.value(key)) {
        colorLight(light, tone);
        light->setAccessibleDescription(text);
        light->setToolTip(text);
    }
    if (auto *badge = m_badges.value(key))
        badge->setStyleSheet(QStringLiteral("QFrame#navigationStatusBadge { background:%1; border:none; border-radius:12px; }")
            .arg(statusColors(tone).halo));
}
void NavigationDiagnosticsPanel::setDetail(const QString &key, const QString &text) {
    if (auto *label = m_details.value(key))
        label->setText(text);
}
void NavigationDiagnosticsPanel::setResource(const QString &key, const QJsonValue &percent, const QString &detail) {
    const bool available = nonnegative(percent) && percent.toDouble() <= 100;
    setValue(key, percentage(percent));
    auto *bar = m_bars.value(key);
    bar->setValue(available ? qRound(percent.toDouble() * 10) : 0);
    bar->setEnabled(available);
    bar->setAccessibleDescription(available ? percentage(percent) : tr("暂无数据"));
    setDetail(key, detail);
    m_details.value(key)->setVisible(!detail.isEmpty());
}
void NavigationDiagnosticsPanel::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    if (m_compact != (width() < 850))
        arrangeCards();
}
void NavigationDiagnosticsPanel::arrangeCards() {
    m_compact = width() < 850;
    for (int i = 0; i < m_summaryCards.size(); ++i) {
        m_summaryGrid->removeWidget(m_summaryCards[i]);
        m_summaryGrid->addWidget(m_summaryCards[i], m_compact ? i : 0, m_compact ? 0 : i);
        m_summaryGrid->setColumnStretch(i, m_compact && i > 0 ? 0 : 1);
    }
    for (int i = 0; i < m_sensorCards.size(); ++i) {
        m_sensorGrid->removeWidget(m_sensorCards[i]);
        m_sensorGrid->addWidget(m_sensorCards[i], m_compact ? i : i / 2, m_compact ? 0 : i % 2);
    }
    m_sensorGrid->setColumnStretch(0, 1);
    m_sensorGrid->setColumnStretch(1, m_compact ? 0 : 1);
    m_bodyGrid->removeWidget(m_sensorSection);
    m_bodyGrid->removeWidget(m_resourceSection);
    m_bodyGrid->addWidget(m_sensorSection, 0, 0);
    m_bodyGrid->addWidget(m_resourceSection, m_compact ? 1 : 0, m_compact ? 0 : 1);
    m_bodyGrid->setColumnStretch(0, m_compact ? 1 : 2);
    m_bodyGrid->setColumnStretch(1, m_compact ? 0 : 1);
}
void NavigationDiagnosticsPanel::configure(const NavigationDiagnosticsConnection &connection) {
    m_connection = connection;
    m_poseHttp.configure(connection.poseUrl, connection.token, connection.poseTimeoutMs);
    m_trackingHttp.configure(connection.trackingUrl, connection.token, connection.trackingTimeoutMs);
    m_resourcesHttp.configure(connection.managementUrl, connection.token, connection.trackingTimeoutMs);
    m_lastSampledAt = 0;
    m_sampleProgressAge.invalidate();
    m_lastResourcesSampledAt = 0;
    m_resourcesProgressAge.invalidate();
    clearResources(tr("等待资源监测连接"));
    clearDiagnostics(connection.poseEnabled ? tr("等待连接") : tr("已禁用位姿服务连接"));
    clearTracking(connection.trackingEnabled ? tr("等待连接") : tr("已禁用任务服务连接"));
    if (isVisible())
        poll();
}
void NavigationDiagnosticsPanel::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    clearDiagnostics(m_connection.poseEnabled ? tr("等待连接") : tr("已禁用位姿服务连接"));
    clearResources(tr("等待资源监测连接"));
    clearTracking(m_connection.trackingEnabled ? tr("等待连接") : tr("已禁用任务服务连接"));
    m_timer.start();
    poll();
}
void NavigationDiagnosticsPanel::hideEvent(QHideEvent *event) {
    m_timer.stop();
    m_poseHttp.cancelAll();
    m_trackingHttp.cancelAll();
    m_resourcesHttp.cancelAll();
    m_lastSampledAt = 0;
    m_sampleProgressAge.invalidate();
    m_lastResourcesSampledAt = 0;
    m_resourcesProgressAge.invalidate();
    clearResources(tr("页面未显示，已停止刷新"));
    clearDiagnostics(tr("页面未显示，已停止刷新"));
    clearTracking(tr("页面未显示，已停止刷新"));
    QWidget::hideEvent(event);
}
void NavigationDiagnosticsPanel::clearDiagnostics(const QString &message) {
    m_diagnosticsAge.invalidate();
    setValue("service", tr("未知"));
    setDetail("service", message);
    setValue("fusion", tr("未知"));
    setDetail("fusion", tr("暂无有效位姿信息"));
    for (const auto &key : {"encoder", "frontRtk", "rearRtk", "imu"}) {
        setValue(key, tr("未知"));
        setDetail(key, tr("暂无有效状态"));
        m_notes.value(key)->setText(tr("等待诊断数据"));
    }
    if (!m_resourcesHttp.configured())
        clearResources(message);
}
void NavigationDiagnosticsPanel::clearResources(const QString &message) {
    m_resourcesAge.invalidate();
    m_resourcesState->setText(message);
    for (const auto &key : {"cpu", "gpu", "memory", "disk"})
        setResource(key, {}, tr("暂无数据"));
    for (const auto &key : {"uptime", "sampledAt"})
        setValue(key, "N/A");
}
void NavigationDiagnosticsPanel::clearTracking(const QString &message) {
    m_healthAge.invalidate();
    m_statusAge.invalidate();
    m_health = {};
    m_status = {};
    setValue("tracking", tr("未知"));
    setDetail("tracking", message);
    setValue("task", tr("未知"));
    m_values.value("task")->setToolTip({});
}
void NavigationDiagnosticsPanel::poll() {
    if (!isVisible())
        return;
    if ((m_resourcesAge.isValid() && m_resourcesAge.elapsed() + m_resourcesReceivedAgeMs > SnapshotLifetimeMs) ||
        (m_resourcesProgressAge.isValid() && m_resourcesProgressAge.elapsed() > SnapshotLifetimeMs))
        clearResources(tr("资源数据已过期"));
    if (m_resourcesHttp.configured())
        m_resourcesHttp.request("resources", "GET", "/management/resources", {}, {}, {}, [this](const JsonHttpResult &result) {
            const auto &object = result.object;
            if (!result.ok() || object.value("apiVersion").toInt() != 1 || object.value("schemaVersion").toInt() != 1 ||
                !nonnegative(object.value("sampledAt")) || object.value("sampledAt").toDouble() <= 0 ||
                !object.value("host").isObject() || !object.value("stale").isBool() ||
                !nonnegative(object.value("snapshotAgeMs"))) {
                clearResources(result.status == 401 ? tr("资源读取认证失败，请核对访问令牌") :
                    result.status == 404 ? tr("资源接口未安装，请更新工控机管理服务") :
                    tr("资源不可用，请检查管理服务和网络连接"));
                return;
            }
            const double sampledAt = object.value("sampledAt").toDouble();
            const bool clockMovedBack = sampledAt < m_lastResourcesSampledAt;
            if (sampledAt != m_lastResourcesSampledAt) {
                m_lastResourcesSampledAt = sampledAt;
                m_resourcesProgressAge.restart();
            }
            const double receivedAgeMs = object.value("snapshotAgeMs").toDouble() + result.roundTripMs;
            if (clockMovedBack || object.value("stale").toBool() ||
                receivedAgeMs > SnapshotLifetimeMs ||
                m_resourcesProgressAge.elapsed() > SnapshotLifetimeMs) {
                clearResources(clockMovedBack ? tr("资源采样时间已变化，等待新状态") : tr("资源数据已过期"));
                return;
            }
            m_resourcesAge.restart();
            m_resourcesReceivedAgeMs = receivedAgeMs;
            renderResources(object);
            m_resourcesState->setText(object.value("status") == "available" ?
                tr("资源监测正常 · 每 2 秒更新") : tr("部分资源指标不可读取"));
        }, 128 * 1024);
    if ((m_diagnosticsAge.isValid() && m_diagnosticsAge.elapsed() > SnapshotLifetimeMs) ||
        (m_sampleProgressAge.isValid() && m_sampleProgressAge.elapsed() > SnapshotLifetimeMs))
        clearDiagnostics(tr("诊断数据已过期"));
    if ((m_healthAge.isValid() && m_healthAge.elapsed() + m_healthReceivedAgeMs > SnapshotLifetimeMs) ||
        (m_statusAge.isValid() && m_statusAge.elapsed() > SnapshotLifetimeMs))
        clearTracking(tr("导航状态已过期"));
    if (m_connection.poseEnabled && m_poseHttp.configured())
        m_poseHttp.request("diagnostics", "GET", "/diagnostics", {}, {}, {}, [this](const JsonHttpResult &result) {
            const auto &object = result.object;
            if (!result.ok() || object.value("schemaVersion").toInt() != 1 ||
                !nonnegative(object.value("sampledAt")) || object.value("sampledAt").toDouble() <= 0 ||
                !object.value("sensors").isObject() ||
                !object.value("host").isObject() || !object.value("fusion").isObject()) {
                clearDiagnostics(result.status == 404 ? tr("诊断接口不支持，请更新工控机服务") : tr("诊断服务不可达或响应无效"));
                return;
            }
            const double sampledAt = object.value("sampledAt").toDouble();
            const bool clockMovedBack = sampledAt < m_lastSampledAt;
            if (sampledAt != m_lastSampledAt) {
                m_lastSampledAt = sampledAt;
                m_sampleProgressAge.restart();
            }
            if (clockMovedBack) {
                clearDiagnostics(tr("采样时间已变化，等待新状态"));
                return;
            }
            const auto snapshotAge = object.value("snapshotAgeMs");
            if (m_sampleProgressAge.elapsed() > SnapshotLifetimeMs ||
                (object.contains("snapshotAgeMs") &&
                    (!nonnegative(snapshotAge) || snapshotAge.toDouble() > SnapshotLifetimeMs))) {
                clearDiagnostics(tr("诊断数据已过期"));
                return;
            }
            m_diagnosticsAge.restart();
            renderDiagnostics(object);
        }, 128 * 1024);
    if (!m_connection.trackingEnabled || !m_trackingHttp.configured())
        return;
    if (readSharedTrackingHealth()) renderTracking();
    else m_trackingHttp.request("health", "GET", "/health", {}, {}, {}, [this](const JsonHttpResult &result) {
        if (readSharedTrackingHealth()) { renderTracking(); return; }
        const auto &object = result.object;
        if (!result.ok() || object.value("apiVersion").toInt() != 1 ||
            object.value("bootId").toString().isEmpty() || !object.value("ready").isBool() || result.roundTripMs > SnapshotLifetimeMs) {
            clearTracking(tr("导航节点不可达或响应无效"));
            return;
        }
        m_health = object;
        m_healthReceivedAgeMs = result.roundTripMs;
        m_healthAge.restart();
        renderTracking();
    }, 128 * 1024);
    if (m_trackingClient) {
        readSharedTrackingStatus();
        return;
    }
    m_trackingHttp.request("status", "GET", "/status", {}, {}, {}, [this](const JsonHttpResult &result) {
        const auto &object = result.object;
        if (!result.ok() || object.value("apiVersion").toInt() != 1 ||
            object.value("bootId").toString().isEmpty() || stateName(object.value("state").toString()).isEmpty()) {
            m_status = {};
            m_statusAge.invalidate();
            setValue("task", tr("任务状态不可达或响应无效"), "warning");
            return;
        }
        m_status = object;
        m_statusAge.restart();
        renderTracking();
    }, 128 * 1024);
}
void NavigationDiagnosticsPanel::renderDiagnostics(const QJsonObject &object) {
    setValue("service", tr("在线"), "ok");
    setDetail("service", tr("已收到有效诊断快照"));
    const auto finalHttp = object.value("fusion").toObject().value("finalHttp").toObject();
    setValue("fusion", sensorState(finalHttp), sensorTone(finalHttp));
    setDetail("fusion", sensorAge(finalHttp));
    m_values.value("fusion")->setToolTip(sensorText(finalHttp));
    const auto sensors = object.value("sensors").toObject();
    for (const auto &key : {"encoder", "frontRtk", "rearRtk", "imu"}) {
        const auto sensor = sensors.value(key).toObject();
        auto detail = sensor.value("valid").isBool() ?
            (sensor.value("valid").toBool() ? tr("数据有效") : tr("数据无效")) : tr("数据有效性未知");
        if (QString::fromLatin1(key).endsWith("Rtk")) {
            const auto satellites = sensor.value("satellites");
            const bool countValid = nonnegative(satellites) && std::floor(satellites.toDouble()) == satellites.toDouble();
            detail = tr("卫星 %1 · %2")
                .arg(countValid ? QString::number(satellites.toDouble(), 'f', 0) : "N/A", fixMode(sensor.value("fixMode")));
        }
        setValue(key, sensorState(sensor), sensorTone(sensor));
        setDetail(key, detail);
        m_notes.value(key)->setText(sensorAge(sensor));
        m_values.value(key)->setToolTip(sensorText(sensor) + "\n" + detail);
    }
    if (!m_resourcesHttp.configured()) {
        renderResources(object);
        m_resourcesState->setText(tr("资源来自融合诊断服务"));
    }
}
void NavigationDiagnosticsPanel::renderResources(const QJsonObject &object) {
    const auto host = object.value("host").toObject();
    setResource("cpu", host.value("cpu").toObject().value("usagePercent"));
    const auto gpu = host.value("gpu").toObject();
    setResource("gpu", gpu.value("status") == "available" ? gpu.value("usagePercent") : QJsonValue(),
                gpu.value("status") == "available" ? QString() : tr("当前不可读取"));
    for (const auto &key : {"memory", "disk"}) {
        const auto resource = host.value(key).toObject();
        setResource(key, resource.value("usedPercent"), availableCapacity(resource));
    }
    const auto uptime = host.value("uptimeSeconds");
    setValue("uptime", nonnegative(uptime) ? tr("%1 小时").arg(uptime.toDouble() / 3600, 0, 'f', 2) : "N/A");
    const auto timestamp = object.value("sampledAt").toDouble();
    // Reject timestamps beyond QDateTime's useful range before converting to an integer.
    setValue("sampledAt", timestamp > 0 && timestamp < 253402300800.0 ?
        QDateTime::fromMSecsSinceEpoch(qint64(timestamp * 1000)).toString("yyyy-MM-dd HH:mm:ss") : "N/A");
}
void NavigationDiagnosticsPanel::renderTracking() {
    if (!m_health.isEmpty()) {
        const bool ready = m_health.value("ready").toBool();
        setValue("tracking", ready ? tr("在线 · 已就绪") : tr("在线 · 未就绪"), ready ? "ok" : "warning");
        setDetail("tracking", ready ? tr("就绪检查通过") : tr("等待导航条件满足"));
    }
    if (m_status.isEmpty())
        return;
    if (!m_health.isEmpty() && m_health.value("bootId") != m_status.value("bootId")) {
        setValue("task", tr("节点重启，等待一致状态"), "warning");
        return;
    }
    const auto state = m_status.value("state").toString();
    auto text = stateName(state);
    if (m_status.value("autoResumePending").toBool())
        text += m_status.value("pauseReason") == "control_cycle_late" ?
            tr(" · 控制调度延迟，停稳后自动继续") : tr(" · 等待传感器/位姿稳定恢复后自动继续");
    else if (state == "Paused")
        text += tr(" · 等待显式继续");
    if (m_status.value("monitorDelayed").toBool()) text += tr(" · 状态延迟");
    const auto reason = state == "Fault" ? m_status.value("faultCode").toString() : m_status.value("pauseReason").toString();
    setValue("task", text, state == "Fault" ? "error" :
             state == "Paused" || state == "Pausing" ? "warning" : QString());
    m_values.value("task")->setToolTip(TrackingJson::reasonText(reason));
}
