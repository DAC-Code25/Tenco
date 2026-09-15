#include "externaleventcoordinator.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <cmath>

ExternalEventCoordinator::ExternalEventCoordinator(TrackingClient *tracking, QObject *parent)
    : QObject(parent), m_tracking(tracking) {
    connect(tracking, &TrackingClient::eventReceived, this, [this](const QJsonObject &event) {
        if (event["type"].toString() != "external_action_requested")
            return;
        if (m_events.size() >= 128)
            m_events.clear();
        m_events.insert(event["eventId"].toString(), event);
    });
    connect(tracking, &TrackingClient::serverRestarted, this, [this] { m_events.clear(); });
    connect(tracking, &TrackingClient::commandUncertain, this, [this](const QString &op, const QString &) {
        if (op == "ack")
            m_ackSent.clear();
    });
    connect(tracking, &TrackingClient::commandFinished, this,
            [this](const QString &op, const QJsonObject &result) {
                if (op == "ack" && result["state"].toString() != "applied")
                    m_ackSent.clear();
            });
    m_timer.setInterval(200);
    connect(&m_timer, &QTimer::timeout, this, &ExternalEventCoordinator::tick);
    m_timer.start();
}
void ExternalEventCoordinator::configure(const QUrl &url, const QString &token, const QString &directory,
                                         int timeout) {
    // Cancelling a photo leaves a durable intent. It must be resolved by the operator, never replayed.
    m_camera.configure(url, token, timeout);
    m_active.clear();
    m_directory = directory;
    QDir().mkpath(m_directory);
}
QString ExternalEventCoordinator::recordPath(const QString &id) const {
    return QDir(m_directory)
        .filePath(
            QString::fromLatin1(QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha256).toHex()) +
            ".json");
}
QJsonObject ExternalEventCoordinator::readRecord(const QString &id) const {
    QFile file(recordPath(id));
    if (!file.exists())
        return {};
    if (!file.open(QIODevice::ReadOnly) || file.size() > 65536)
        return {{"state", "unknown"}};
    const auto object = QJsonDocument::fromJson(file.readAll()).object();
    if (object["eventId"].toString() != id)
        return {{"state", "unknown"}};
    return object;
}
bool ExternalEventCoordinator::writeRecord(const QJsonObject &record) {
    QSaveFile file(recordPath(record["eventId"].toString()));
    if (!QFile::exists(file.fileName()) &&
        QDir(m_directory).entryList({"*.json"}, QDir::Files).size() >= 4096) {
        emit message(tr("检查点动作记录已达容量上限，请归档已结束任务的记录后继续"));
        return false;
    }
    const auto bytes = QJsonDocument(record).toJson(QJsonDocument::Compact);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        emit message(tr("检查点动作记录保存失败，保持等待，请检查磁盘"));
        return false;
    }
    return true;
}
bool ExternalEventCoordinator::resolveCurrent(const QString &expectedEventId, bool success) {
    const auto status = m_tracking->snapshot();
    if (!m_tracking->fresh() || !m_tracking->hasSession() || status.waitingEventId.isEmpty() ||
        status.waitingEventId != expectedEventId || !m_active.isEmpty() ||
        (status.state != "Waiting" && status.state != "Paused"))
        return false;
    const auto savedState = readRecord(expectedEventId).value("state").toString();
    if (savedState == "succeeded" || savedState == "failed" || m_ackSent == expectedEventId)
        return false;
    QJsonObject record{{"eventId", status.waitingEventId},
                       {"executionId", status.executionId},
                       {"stepId", status.stepId},
                       {"state", success ? "succeeded" : "failed"},
                       {"resolution", "operator"}};
    m_ackSent.clear();
    return writeRecord(record);
}
void ExternalEventCoordinator::tick() {
    const auto status = m_tracking->snapshot();
    const auto id = status.waitingEventId;
    if (m_directory.isEmpty() || id.isEmpty() || !m_tracking->fresh() || !m_tracking->hasSession() ||
        m_tracking->busy() || (status.state != "Waiting" && status.state != "Paused"))
        return;
    auto record = readRecord(id);
    const auto state = record.value("state").toString();
    QJsonObject event{{"eventId", id}, {"executionId", status.executionId}, {"stepId", status.stepId}};
    if (state == "succeeded" || state == "failed") {
        if (m_ackSent != id && m_tracking->acknowledge(event, state == "succeeded"))
            m_ackSent = id;
        return;
    }
    if (!m_active.isEmpty())
        return;
    const auto received = m_events.value(id);
    const bool ownsPreparedTask = m_tracking->preparedPlan()["taskId"].toString() == status.taskId &&
                                  m_tracking->preparedPlan()["revision"].toInt() == status.taskRevision;
    if (!record.isEmpty() || received.isEmpty() || !ownsPreparedTask ||
        received["reason"].toString() != "capture") {
        if (m_reported != id) {
            m_reported = id;
            emit message(tr("检查点动作待核对：%1。请核查拍摄结果后明确确认成功或失败").arg(id));
        }
        return;
    }
    if (status.state != "Waiting" || std::abs(status.measuredV) > .02 || std::abs(status.measuredOmega) > .03)
        return;
    record = event;
    record["state"] = "intent";
    if (!m_camera.configured() || m_directory.isEmpty() || !writeRecord(record))
        return;
    m_active = id;
    if (!m_camera.request("photo", "POST", "/camera/photo", {{"eventId", id}}, {}, {},
                          [this, id, record](const JsonHttpResult &r) mutable {
                              const bool success = r.ok() && r.object["success"].isBool() &&
                                                   r.object["success"].toBool() &&
                                                   !r.object["path"].toString().isEmpty();
                              record["state"] = success ? "succeeded" : "unknown";
                              record["path"] = r.object["path"].toString();
                              writeRecord(record);
                              m_active.clear();
                              emit message(success ? tr("检查点拍摄完成，正在确认结果")
                                                   : tr("检查点拍摄结果未知，请人工核对；不会自动重拍"));
                          })) {
        m_active.clear();
    }
}
