#include "trackingclient.h"
#include <QJsonArray>

TrackingClient::TrackingClient(QObject *parent) : QObject(parent), m_clientId(TrackingJson::newId()) {
    m_clock.start();
    m_timer.setInterval(50);
    connect(&m_timer, &QTimer::timeout, this, &TrackingClient::tick);
}
void TrackingClient::configure(const QUrl &url, const QString &token, int timeout) {
    const bool restart = m_running;
    stop();
    m_http.configure(url, token, timeout);
    if (restart)
        start();
}
void TrackingClient::start() {
    m_running = true;
    m_renewSession = true;
    m_timer.start();
    tick();
}
void TrackingClient::stop() {
    m_running = false;
    m_timer.stop();
    m_http.cancelAll();
    loseSession();
    for (const auto &p : std::as_const(m_pending))
        emit commandUncertain(p.operation, p.requestId);
    m_pending.clear();
    m_status = {};
    m_statusAge.invalidate();
    m_configuration = {};
    m_health = {};
    m_preparedPlan = {};
    m_eventCursor = 0;
    m_nextMetadata = m_nextStatus = m_nextEvents = 0;
    m_online = false;
    emit availabilityChanged(false, tr("任务服务未连接"));
}
bool TrackingClient::fresh() const {
    return m_running && m_statusAge.isValid() && m_statusAge.elapsed() + m_statusRoundTripMs <= 300;
}
bool TrackingClient::hasSession() const {
    return m_running && !m_sessionId.isEmpty() && m_clock.elapsed() < m_sessionDeadline;
}
bool TrackingClient::motionReady() const {
    return fresh() && hasSession() && !m_permit.isEmpty() && m_clock.elapsed() + 50 < m_permitDeadline &&
           m_sessionBoot == m_status.bootId && m_permitVersion == m_status.stateVersion &&
           m_health["ready"].toBool() && !busy();
}
void TrackingClient::loseSession() {
    const bool had = !m_sessionId.isEmpty();
    m_sessionId.clear();
    m_sessionToken.clear();
    m_sessionBoot.clear();
    m_permit.clear();
    m_sessionDeadline = m_permitDeadline = 0;
    if (had)
        emit sessionChanged(false);
}
void TrackingClient::acquireSession() {
    if (!m_running || hasSession())
        return;
    QJsonObject body{{"clientId", m_clientId}, {"requestId", TrackingJson::newId()}};
    m_http.request("session", "POST", "/sessions", body, {}, {}, [this](const JsonHttpResult &r) {
        if (!r.ok() || r.object["apiVersion"].toInt() != 1 || r.object["sessionId"].toString().isEmpty() ||
            r.object["sessionToken"].toString().isEmpty() ||
            r.object["bootId"].toString() != m_status.bootId) {
            emit errorOccurred(r.error.isEmpty() ? "invalid_session_response" : r.error);
            return;
        }
        m_sessionId = r.object["sessionId"].toString();
        m_sessionToken = r.object["sessionToken"].toString();
        m_sessionBoot = r.object["bootId"].toString();
        m_sessionDeadline =
            m_clock.elapsed() + qBound(0, r.object["expiresInMs"].toInt() - int(r.roundTripMs), 1000);
        m_nextHeartbeat = 0;
        heartbeat();
        emit sessionChanged(hasSession());
    });
}
void TrackingClient::heartbeat() {
    if (!m_renewSession || !hasSession() || m_clock.elapsed() < m_nextHeartbeat)
        return;
    m_nextHeartbeat = m_clock.elapsed() + 200;
    const auto session = m_sessionId;
    m_http.request(
        "heartbeat", "POST", "/sessions/" + session + "/heartbeat", {{"requestId", TrackingJson::newId()}},
        {}, m_sessionToken, [this, session](const JsonHttpResult &r) {
            if (m_sessionId != session)
                return;
            quint64 version = 0;
            if (!r.ok() || r.object["apiVersion"].toInt() != 1 ||
                r.object["bootId"].toString() != m_sessionBoot ||
                r.object["sessionId"].toString() != session ||
                !TrackingJson::sequence(r.object["stateVersion"], &version)) {
                loseSession();
                emit errorOccurred(r.error.isEmpty() ? "invalid_heartbeat_response" : r.error);
                return;
            }
            m_sessionDeadline = m_clock.elapsed() + qMax<qint64>(0, 1000 - r.roundTripMs);
            m_permit = r.object["motionPermit"].toString();
            m_permitVersion = version;
            m_permitDeadline =
                m_clock.elapsed() +
                qBound(qint64(0), r.object["permitValidForMs"].toInt() - r.roundTripMs, qint64(500));
        });
}
void TrackingClient::tick() {
    if (!m_running)
        return;
    if (!m_sessionId.isEmpty() && !hasSession())
        loseSession();
    const bool online = fresh();
    if (online != m_online) {
        m_online = online;
        emit availabilityChanged(online, online ? QString() : tr("任务状态过期，执行状态未知"));
    }
    heartbeat();
    pollStatus();
    pollCommands();
    pollEvents();
    pollMetadata();
}
void TrackingClient::pollStatus() {
    if (m_clock.elapsed() < m_nextStatus)
        return;
    QUrlQuery query;
    query.addQueryItem("bootId", m_status.bootId);
    query.addQueryItem("since", QString::number(m_status.seq));
    query.addQueryItem("waitMs", "100");
    m_http.request("status", "GET", "/status", {}, query, {}, [this](const JsonHttpResult &r) {
        if (!r.ok()) {
            m_statusAge.invalidate();
            m_permit.clear();
            m_nextStatus = m_clock.elapsed() + qMin(5000, 100 * (1 << qMin(++m_failures, 5)));
            return;
        }
        m_failures = 0;
        if (r.status == 204)
            return;
        TrackingSnapshot status;
        QString error;
        if (!TrackingJson::status(r.object, &status, &error)) {
            m_statusAge.invalidate();
            emit errorOccurred(error);
            return;
        }
        if (status.bootId == m_status.bootId && status.seq <= m_status.seq)
            return;
        const bool reboot = !m_status.bootId.isEmpty() && m_status.bootId != status.bootId;
        if (reboot) {
            loseSession();
            m_preparedPlan = {};
            m_configuration = {};
            m_health = {};
            m_eventCursor = 0;
            for (const auto &p : std::as_const(m_pending))
                emit commandUncertain(p.operation, p.requestId);
            m_pending.clear();
            emit serverRestarted();
            m_nextMetadata = 0;
        }
        m_status = status;
        m_statusRoundTripMs = r.roundTripMs;
        m_statusAge.restart();
        if (!m_status.waitingEventId.isEmpty() && m_eventCursor == 0)
            emit eventsResynchronized();
        emit statusChanged(status);
    });
}
void TrackingClient::pollMetadata() {
    if (m_clock.elapsed() < m_nextMetadata)
        return;
    m_nextMetadata = m_clock.elapsed() + 1000;
    m_http.request("health", "GET", "/health", {}, {}, {}, [this](const JsonHttpResult &r) {
        if (r.ok() && r.object["apiVersion"].toInt() == 1 && r.object["bootId"].toString() == m_status.bootId)
            m_health = r.object;
        else
            m_health = {};
    });
    m_http.request("configuration", "GET", "/configuration", {}, {}, {}, [this](const JsonHttpResult &r) {
        if (r.ok() && r.object["apiVersion"].toInt() == 1 && r.object != m_configuration) {
            m_configuration = r.object;
            emit configurationChanged(r.object);
        }
    });
}
bool TrackingClient::sendCommand(const QString &path, QJsonObject body, const QString &operation,
                                 bool safety) {
    if (!m_running || m_pending.size() >= 16 || (!safety && !hasSession()))
        return false;
    const auto id = TrackingJson::newId();
    const QString session = operation == "estop" ? "emergency" : m_sessionId;
    body["requestId"] = id;
    body["sessionId"] = session;
    const bool sent = m_http.request(
        safety ? "safety" : "write", "POST", path, body, {}, m_sessionToken,
        [this, id](const JsonHttpResult &r) {
            if (!m_pending.contains(id))
                return;
            if (r.ok() && !r.object["commandId"].toString().isEmpty())
                m_pending[id].commandId = r.object["commandId"].toString();
            else if (r.status >= 400 && r.status < 500 && r.status != 429) {
                const auto p = m_pending.take(id);
                emit commandFinished(p.operation,
                                     {{"state", "rejected"}, {"reason", r.error}, {"requestId", id}});
            }
            // A lost response, 503 or 429 can occur after acceptance. Query this request only.
        });
    if (sent)
        m_pending.insert(id, {id, {}, operation, session, m_clock.elapsed()});
    return sent;
}
bool TrackingClient::upload(const QJsonObject &plan) {
    if (!fresh() || busy() || !hasSession())
        return false;
    if (!sendCommand("/tasks", plan, "prepare"))
        return false;
    m_preparedPlan = plan;
    return true;
}
bool TrackingClient::control(const QString &operation) {
    const bool emergency = operation == "estop";
    const bool motion = operation == "start" || operation == "resume";
    if (!emergency && (!fresh() || !hasSession()))
        return false;
    if (motion && !motionReady())
        return false;
    QJsonObject body{{"cmd", operation},
                     {"taskId", m_status.taskId},
                     {"revision", m_status.taskRevision},
                     {"executionId",
                      operation == "start" ? QJsonValue(QJsonValue::Null) : QJsonValue(m_status.executionId)},
                     {"expectedStateVersion", QString::number(m_status.stateVersion)}};
    if (motion) {
        if ((operation == "start" && m_status.state != "Ready") ||
            (operation == "resume" && m_status.state != "Paused"))
            return false;
        if (m_preparedPlan["taskId"].toString() != m_status.taskId ||
            m_preparedPlan["revision"].toInt() != m_status.taskRevision)
            return false;
        body["expectedChassisBootId"] = m_status.chassisBootId;
        body["expectedOwnerEpoch"] = QString::number(m_status.ownerEpoch);
        body["motionPermit"] = m_permit;
        body["validForMs"] = 500;
    }
    const bool sent = sendCommand("/control", body, operation,
                                  emergency || operation == "pause" || operation == "abort" ||
                                      operation == "manual_takeover");
    if (sent && motion)
        m_permit.clear();
    return sent;
}
void TrackingClient::pollCommands() {
    if (m_pending.isEmpty() || m_http.busy("result"))
        return;
    auto it = m_pending.begin();
    // Round-robin by last query time prevents one unknown request starving later stops.
    for (auto candidate = m_pending.begin(); candidate != m_pending.end(); ++candidate)
        if (candidate->queriedAt < it->queriedAt)
            it = candidate;
    it->queriedAt = m_clock.elapsed();
    const auto p = it.value();
    QUrlQuery query;
    QString path;
    if (p.commandId.isEmpty()) {
        path = "/commands";
        query.addQueryItem("requestId", p.requestId);
        query.addQueryItem("sessionId", p.sessionId);
    } else
        path = "/commands/" + p.commandId;
    m_http.request("result", "GET", path, {}, query, {}, [this, p](const JsonHttpResult &r) {
        if (!m_pending.contains(p.requestId))
            return;
        if (r.ok() && QStringList{"applied", "rejected", "expired"}.contains(r.object["state"].toString())) {
            m_pending.remove(p.requestId);
            emit commandFinished(p.operation, r.object);
            return;
        }
        if (r.ok())
            m_pending[p.requestId].commandId = r.object["commandId"].toString();
        if (m_clock.elapsed() - p.createdAt > 10000) {
            m_pending.remove(p.requestId);
            emit commandUncertain(p.operation, p.requestId);
        }
    });
}
void TrackingClient::pollEvents() {
    if (!fresh() || m_clock.elapsed() < m_nextEvents)
        return;
    m_nextEvents = m_clock.elapsed() + 200;
    QUrlQuery q;
    q.addQueryItem("after", QString::number(m_eventCursor));
    q.addQueryItem("waitMs", "100");
    const auto boot = m_status.bootId;
    m_http.request("events", "GET", "/events", {}, q, {}, [this, boot](const JsonHttpResult &r) {
        if (boot != m_status.bootId)
            return;
        if (r.status == 410) {
            m_eventCursor = m_status.lastEventSeq;
            emit eventsResynchronized();
            if (!m_status.executionId.isEmpty())
                m_http.request("execution", "GET", "/executions/" + m_status.executionId, {}, {}, {},
                               [this](const JsonHttpResult &result) {
                                   if (result.ok())
                                       emit executionReceived(result.object);
                               });
            return;
        }
        if (!r.ok() || r.status == 204 || r.object["apiVersion"].toInt() != 1 ||
            r.object["bootId"].toString() != boot)
            return;
        for (const auto &value : r.object["events"].toArray()) {
            const auto event = value.toObject();
            quint64 seq = 0;
            if (!TrackingJson::sequence(event["seq"], &seq) || seq <= m_eventCursor)
                continue;
            m_eventCursor = seq;
            emit eventReceived(event);
        }
    });
}
bool TrackingClient::acknowledge(const QJsonObject &event, bool success) {
    const auto id = event["eventId"].toString();
    if (!fresh() || id.isEmpty() || id != m_status.waitingEventId ||
        event["executionId"].toString() != m_status.executionId ||
        event["stepId"].toString() != m_status.stepId)
        return false;
    return sendCommand(
        "/events/" + id + "/ack",
        {{"executionId", m_status.executionId}, {"stepId", m_status.stepId}, {"success", success}}, "ack");
}
bool TrackingClient::updateOrigin(const QJsonObject &origin, const QString &revision) {
    if (!fresh() || !hasSession() || busy())
        return false;
    return m_http.request("origin", "POST", "/origin",
                          {{"requestId", TrackingJson::newId()},
                           {"sessionId", m_sessionId},
                           {"expectedRevision", revision},
                           {"origin", origin}},
                          {}, m_sessionToken, [this](const JsonHttpResult &r) {
                              if (r.ok())
                                  emit originChanged(r.object);
                              else
                                  emit errorOccurred(r.error);
                          });
}
void TrackingClient::readTask(const QString &id, int revision) {
    QUrlQuery query;
    query.addQueryItem("revision", QString::number(revision));
    m_http.request("task", "GET", "/tasks/" + id, {}, query, {}, [this](const JsonHttpResult &r) {
        if (r.ok())
            emit taskReceived(r.object);
        else
            emit errorOccurred(r.error);
    });
}
