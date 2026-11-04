#include "trackingclient.h"
#include <QJsonArray>
#include <QRegularExpression>
#include <QCryptographicHash>
#include <cmath>

namespace {
bool transientFailure(const JsonHttpResult &result) {
    if (result.error == "request_timeout" || result.error == "connection_failed") return true;
    if (result.error == "invalid_json_response" || result.error == "response_too_large") return false;
    return result.status == 408 || result.status == 429 || (result.status >= 500 && result.status <= 599);
}
bool boundedMilliseconds(const QJsonValue &value, int maximum) {
    const double number = value.toDouble();
    return value.isDouble() && std::isfinite(number) && number == std::floor(number) &&
           number > 0 && number <= maximum;
}
}

TrackingClient::TrackingClient(QObject *parent) : QObject(parent), m_clientId(TrackingJson::newId()) {
    m_clock.start();
    m_timer.setInterval(50);
    connect(&m_timer, &QTimer::timeout, this, &TrackingClient::tick);
}
void TrackingClient::configure(const QUrl &url, const QString &token, int timeout) {
    if (m_http.matchesConfiguration(url, token, timeout)) return;
    const bool restart = m_running;
    stop();
    m_http.configure(url, token, timeout);
    m_safetyHttp.configure(url, token, timeout);
    if (restart)
        start();
}
void TrackingClient::start() {
    if (m_running) return;
    m_running = true;
    m_renewSession = true;
    m_timer.start();
    tick();
}
void TrackingClient::stop() {
    ++m_metadataGeneration;
    m_historyRead.reset();
    m_historyContextBoot.clear(); m_historyContextTask.clear(); m_historyContextExecution.clear();
    m_historyContextRevision = 0;
    m_historyAttempts = 0;
    m_running = false;
    m_timer.stop();
    m_http.cancelAll();
    m_safetyHttp.cancelAll();
    loseSession();
    for (const auto &p : std::as_const(m_pending))
        emit commandUncertain(p.operation, p.requestId);
    const bool hadPending = busy();
    m_pending.clear();
    if (hadPending) emit pendingChanged(false);
    m_status = {};
    m_statusAge.invalidate();
    m_statusUsable = false;
    m_statusTransportHealthy = false;
    m_configuration = {};
    m_configurationAge.invalidate();
    m_health = {};
    m_healthAge.invalidate();
    invalidatePreparedPlan();
    m_eventCursor = 0;
    m_nextHealth = m_nextConfiguration = m_nextStatus = m_nextEvents = 0;
    m_nextCommandResult = m_nextSafetyResult = 0;
    m_online = m_connected = m_commandFresh = false;
    emit availabilityChanged(false, tr("任务服务未连接"));
}
bool TrackingClient::fresh() const {
    // This is the wireless monitoring link. The IPC checks its local pose and
    // feedback deadlines independently before accepting a motion request.
    return m_running && m_statusUsable && m_statusAge.isValid() && m_statusAge.elapsed() + m_statusRoundTripMs <= 1000;
}
qint64 TrackingClient::healthAgeMs() const {
    if (!m_running || !m_healthAge.isValid() || m_health.isEmpty() ||
        m_status.bootId.isEmpty() || m_health.value("bootId").toString() != m_status.bootId) return -1;
    return m_healthAge.elapsed() + m_healthRoundTripMs;
}
bool TrackingClient::healthFresh() const {
    const auto age = healthAgeMs();
    return age >= 0 && age <= 2000;
}
bool TrackingClient::connected() const {
    // A wireless monitoring gap does not stop the autonomous IPC task. Keep
    // connectivity separate from the fresh snapshot required for new writes.
    return m_running && m_statusAge.isValid() && m_statusAge.elapsed() + m_statusRoundTripMs <= 5000;
}
bool TrackingClient::commandFresh() const {
    // A bounded wireless snapshot is sufficient for an explicit operation;
    // the IPC independently verifies its boot, state version and live inputs.
    return m_running && m_statusUsable && m_statusAge.isValid() && m_statusAge.elapsed() + m_statusRoundTripMs <= 3000;
}
bool TrackingClient::hasSession() const {
    return m_running && !m_sessionId.isEmpty() && m_clock.elapsed() < m_sessionDeadline;
}
bool TrackingClient::motionReady() const {
    return commandFresh() && configurationFresh() && m_statusTransportHealthy && hasSession() && !m_permit.isEmpty() && m_clock.elapsed() + 50 < m_permitDeadline &&
           m_sessionBoot == m_status.bootId && m_permitVersion == m_status.stateVersion &&
           m_health["ready"].toBool() && !busy();
}
bool TrackingClient::configurationFresh() const {
    // Keep the last snapshot available for display. A failed or overdue refresh
    // cannot authorize a task using that cached configuration.
    return m_running && m_configurationAge.isValid() &&
           m_configurationAge.elapsed() + m_configurationRoundTripMs <= 6000;
}
void TrackingClient::refreshConfiguration() {
    ++m_configurationGeneration;
    m_configurationAge.invalidate();
    m_nextConfiguration = 0;
    if (m_running) pollMetadata();
}
bool TrackingClient::canResetFault() const {
    // The coordinator acquires the operation session after this explicit click.
    // A reset clears the old task; it never grants permission to resume motion.
    return commandFresh() && m_status.state == "Fault" && !busy() &&
           m_status.chassisConnected && m_status.measuredValid && m_status.measuredStopped;
}
void TrackingClient::loseSession() {
    const bool had = !m_sessionId.isEmpty();
    m_sessionId.clear();
    m_sessionToken.clear();
    m_sessionBoot.clear();
    revokeMotionPermit();
    m_sessionDeadline = m_permitDeadline = 0;
    if (had)
        emit sessionChanged(false);
}
void TrackingClient::revokeMotionPermit() {
    ++m_permitGeneration;
    m_permit.clear();
    m_permitDeadline = 0;
}
bool TrackingClient::acquireSession() {
    if (!m_running || hasSession() || m_http.busy("session"))
        return false;
    // An explicit new acquisition re-enables renewal after maintenance release.
    m_renewSession = true;
    QJsonObject body{{"clientId", m_clientId}, {"requestId", TrackingJson::newId()}};
    return m_http.request("session", "POST", "/sessions", body, {}, {}, [this](const JsonHttpResult &r) {
        if (!r.ok() || r.object["apiVersion"].toInt() != 1 || r.object["sessionId"].toString().isEmpty() ||
            r.object["sessionToken"].toString().isEmpty() ||
            r.object["bootId"].toString() != m_status.bootId) {
            emit errorOccurred(r.error.isEmpty() ? "invalid_session_response" : r.error);
            return;
        }
        m_sessionId = r.object["sessionId"].toString();
        m_sessionToken = r.object["sessionToken"].toString();
        m_sessionBoot = r.object["bootId"].toString();
        m_sessionLeaseMs = qBound(0, r.object["expiresInMs"].toInt(), 10000);
        m_sessionDeadline = m_clock.elapsed() + qMax<qint64>(0, m_sessionLeaseMs - r.roundTripMs);
        m_nextHeartbeat = 0;
        heartbeat();
        emit sessionChanged(hasSession());
    });
}
bool TrackingClient::releaseSessionForMaintenance(QString *message) {
    const bool safeState = m_status.state == "Idle" || m_status.state == "Ready" || m_status.isTerminal();
    if (!commandFresh() || !safeState || m_status.autoResumePending || busy() || m_http.busy("session")) {
        if (message) *message = tr("当前无法释放操作权：请先结束任务，并确认导航状态新鲜、任务空闲且没有待确认操作。暂停任务不能释放。此操作不会停止或中止任务。");
        return false;
    }
    m_renewSession = false;
    loseSession();
    refreshConfiguration();
    if (message) *message = tr("已停止上位机操作权续期，请等待最多 %1 秒后重试维护操作。再次操作任务时将自动获取操作权。")
        .arg((m_sessionLeaseMs + 999) / 1000);
    return true;
}
void TrackingClient::heartbeat() {
    if (!m_renewSession || !hasSession() || m_clock.elapsed() < m_nextHeartbeat)
        return;
    m_nextHeartbeat = m_clock.elapsed() + 200;
    const auto session = m_sessionId;
    const auto permitGeneration = m_permitGeneration;
    m_http.request(
        "heartbeat", "POST", "/sessions/" + session + "/heartbeat", {{"requestId", TrackingJson::newId()}},
        {}, m_sessionToken, [this, session, permitGeneration](const JsonHttpResult &r) {
            if (m_sessionId != session)
                return;
            if (!r.ok() && transientFailure(r)) {
                // A lost renewal may already have renewed the server lease.
                // Keep only our previously known deadline; never invent an extension.
                revokeMotionPermit();
                return;
            }
            quint64 version = 0;
            if (!r.ok() || r.object["apiVersion"].toInt() != 1 ||
                r.object["bootId"].toString() != m_sessionBoot ||
                r.object["sessionId"].toString() != session ||
                r.object["motionPermit"].toString().isEmpty() ||
                !boundedMilliseconds(r.object["permitValidForMs"], 3000) ||
                (r.object.contains("expiresInMs") && !boundedMilliseconds(r.object["expiresInMs"], 10000)) ||
                !TrackingJson::sequence(r.object["stateVersion"], &version)) {
                loseSession();
                emit errorOccurred(r.error.isEmpty() ? "invalid_heartbeat_response" : r.error);
                return;
            }
            if (r.object.contains("expiresInMs"))
                m_sessionLeaseMs = qBound(0, r.object["expiresInMs"].toInt(), 10000);
            m_sessionDeadline = m_clock.elapsed() + qMax<qint64>(0, m_sessionLeaseMs - r.roundTripMs);
            if (permitGeneration != m_permitGeneration || !m_statusTransportHealthy) return;
            m_permit = r.object["motionPermit"].toString();
            m_permitVersion = version;
            m_permitDeadline =
                m_clock.elapsed() +
                qMax<qint64>(0, qBound(0, r.object["permitValidForMs"].toInt(), 3000) - r.roundTripMs);
        });
}
void TrackingClient::tick() {
    if (!m_running)
        return;
    if (!m_sessionId.isEmpty() && !hasSession())
        loseSession();
    const bool online = fresh();
    const bool connection = connected();
    const bool usable = commandFresh();
    if (online != m_online || connection != m_connected || usable != m_commandFresh) {
        m_online = online;
        m_connected = connection;
        m_commandFresh = usable;
        emit availabilityChanged(online, online ? QString() : connection ?
            tr("无线状态更新延迟，显示上次状态；工控机独立执行，操作仍由服务端校验") :
            tr("任务监视连接中断，执行状态未知；工控机按本地状态独立执行"));
    }
    heartbeat();
    pollStatus();
    pollCommands();
    pollEvents();
    pollMetadata();
}
void TrackingClient::pollStatus() {
    if (m_clock.elapsed() < m_nextStatus || m_http.busy("status"))
        return;
    QUrlQuery query;
    query.addQueryItem("bootId", m_status.bootId);
    query.addQueryItem("since", QString::number(m_status.seq));
    query.addQueryItem("waitMs", "100");
    m_http.request("status", "GET", "/status", {}, query, {}, [this](const JsonHttpResult &r) {
        if (!r.ok()) {
            // A single lost monitor response does not turn a recent snapshot
            // into a disconnected service. Its real age continues to advance.
            m_statusTransportHealthy = false;
            revokeMotionPermit();
            if (!transientFailure(r)) {
                m_statusUsable = false;
                m_statusAge.invalidate();
                loseSession();
                emit errorOccurred(r.error);
            }
            m_nextStatus = m_clock.elapsed() + qMin(5000, 100 * (1 << qMin(++m_failures, 5)));
            return;
        }
        // Wait between completed polls as RTT grows. There is still only one
        // status request in flight; wireless delay never grants fresher state.
        m_nextStatus = m_clock.elapsed() + qBound(qint64(100), r.roundTripMs, qint64(500));
        m_failures = 0;
        if (r.status == 204)
            return;
        TrackingSnapshot status;
        QString error;
        if (!TrackingJson::status(r.object, &status, &error)) {
            m_statusUsable = false;
            m_statusTransportHealthy = false;
            m_statusAge.invalidate();
            loseSession();
            emit errorOccurred(error);
            return;
        }
        if (status.bootId == m_status.bootId && status.seq <= m_status.seq)
            return;
        const bool recovered = !m_status.bootId.isEmpty() && (!m_statusTransportHealthy || !commandFresh());
        const bool reboot = !m_status.bootId.isEmpty() && m_status.bootId != status.bootId;
        if (reboot) {
            ++m_metadataGeneration;
            m_historyRead.reset();
            loseSession();
            invalidatePreparedPlan();
            m_configuration = {};
            m_configurationAge.invalidate();
            emit configurationChanged({});
            m_health = {};
            m_healthAge.invalidate();
            m_eventCursor = 0;
            for (const auto &p : std::as_const(m_pending))
                emit commandUncertain(p.operation, p.requestId);
            const bool hadPending = busy();
            m_pending.clear();
            if (hadPending) emit pendingChanged(false);
            emit serverRestarted();
            m_nextHealth = m_nextConfiguration = 0;
        }
        m_status = status;
        m_statusUsable = true;
        m_statusTransportHealthy = true;
        m_statusRoundTripMs = r.roundTripMs;
        m_statusAge.restart();
        if (recovered || reboot) {
            m_nextHealth = 0;
            refreshConfiguration();
        }
        if (!m_status.waitingEventId.isEmpty() && m_eventCursor == 0)
            emit eventsResynchronized();
        emit statusChanged(status);
    });
}
void TrackingClient::pollMetadata() {
    if (!commandFresh() || !m_statusTransportHealthy)
        return;
    const auto boot = m_status.bootId;
    const auto generation = m_metadataGeneration;
    if (m_clock.elapsed() >= m_nextHealth && !m_http.busy("health") &&
        m_http.request("health", "GET", "/health", {}, {}, {}, [this, boot, generation](const JsonHttpResult &r) {
        if (generation != m_metadataGeneration || boot != m_status.bootId) return;
        if (r.ok() && r.object["apiVersion"].toInt() == 1 && r.object["bootId"].toString() == boot && r.object["ready"].isBool()) {
            m_health = r.object;
            m_healthRoundTripMs = r.roundTripMs;
            m_healthAge.restart();
        } else {
            m_health = {};
            m_healthAge.invalidate();
        }
    })) m_nextHealth = m_clock.elapsed() + 1000;
    const auto configurationGeneration = m_configurationGeneration;
    if (m_clock.elapsed() >= m_nextConfiguration && !m_http.busy("configuration") &&
        m_http.request("configuration", "GET", "/configuration", {}, {}, {},
                       [this, boot, generation, configurationGeneration](const JsonHttpResult &r) {
        if (generation != m_metadataGeneration || configurationGeneration != m_configurationGeneration ||
            boot != m_status.bootId) return;
        if (r.ok() && r.object["apiVersion"].toInt() == 1 &&
            (!r.object.contains("bootId") || r.object["bootId"].toString() == boot)) {
            m_configurationAge.restart();
            m_configurationRoundTripMs = r.roundTripMs;
            if (r.object != m_configuration) {
                m_configuration = r.object;
                emit configurationChanged(r.object);
            }
        } else {
            m_configurationAge.invalidate();
            m_nextConfiguration = m_clock.elapsed() + 1000;
        }
    })) m_nextConfiguration = m_clock.elapsed() + 5000;
}
bool TrackingClient::sendCommand(const QString &path, QJsonObject body, const QString &operation,
                                 bool safety, QString *requestId) {
    if (requestId) requestId->clear();
    if (!m_running || (!safety && (m_pending.size() >= 16 || !hasSession())))
        return false;
    // Reserve one bounded request per stop operation; a pending pause must not block estop.
    if (safety)
        for (const auto &pending : std::as_const(m_pending))
            if (pending.safety && pending.operation == operation)
                return false;
    const auto id = TrackingJson::newId();
    const QString session = operation == "estop" ? "emergency" : safety ? "safety" : m_sessionId;
    body["requestId"] = id;
    body["sessionId"] = session;
    auto &http = safety ? m_safetyHttp : m_http;
    const bool sent = http.request(
        safety ? operation : "write", "POST", path, body, {}, safety ? QString() : m_sessionToken,
        [this, id](const JsonHttpResult &r) {
            if (!m_pending.contains(id))
                return;
            m_pending[id].postPending = false;
            if (!m_pending[id].safety) m_pending[id].nextQueryAt = m_clock.elapsed() + 100;
            if (r.ok() && !r.object["commandId"].toString().isEmpty())
                noteAccepted(id, r.object["commandId"].toString());
            else if (r.status >= 400 && r.status < 500 && r.status != 429) {
                const auto p = m_pending.take(id);
                if (!busy()) emit pendingChanged(false);
                emit commandFinished(p.operation,
                                     {{"state", "rejected"}, {"reason", r.error}, {"requestId", id}});
            }
            // A lost response, 503 or 429 can occur after acceptance. Query this request only.
        });
    if (sent) {
        const bool wasPending = busy();
        Pending pending;
        pending.requestId = id; pending.operation = operation; pending.sessionId = session;
        pending.createdAt = m_clock.elapsed(); pending.nextQueryAt = pending.createdAt + 100;
        pending.safety = safety;
        m_pending.insert(id, pending);
        if (requestId) *requestId = id;
        if (!wasPending) emit pendingChanged(true);
    }
    return sent;
}
bool TrackingClient::upload(const QJsonObject &plan, QString *requestId) {
    if (requestId) requestId->clear();
    if (!commandFresh() || !configurationFresh() || busy() || !hasSession())
        return false;
    if (!sendCommand("/tasks", plan, "prepare", false, requestId))
        return false;
    m_preparedPlan = plan;
    return true;
}
bool TrackingClient::control(const QString &operation, QString *requestId) {
    if (requestId) requestId->clear();
    const bool emergency = operation == "estop";
    const bool stop = operation == "pause" || operation == "abort";
    const bool motion = operation == "start" || operation == "resume";
    if (!emergency && !stop && (!commandFresh() || !hasSession()))
        return false;
    if (stop && (m_status.bootId.isEmpty() || m_status.taskId.isEmpty() || m_status.taskRevision < 1 ||
                 m_status.executionId.isEmpty()))
        return false;
    if (motion && !motionReady())
        return false;
    if (operation == "reset_fault" && !canResetFault())
        return false;
    QJsonObject body{{"cmd", operation},
                     {"taskId", m_status.taskId},
                     {"revision", m_status.taskRevision},
                     {"executionId",
                      operation == "start" ? QJsonValue(QJsonValue::Null) : QJsonValue(m_status.executionId)},
                     {"expectedStateVersion", QString::number(m_status.stateVersion)}};
    if (stop) {
        body["safetyStop"] = true;
        body["bootId"] = m_status.bootId;
    }
    if (motion) {
        if ((operation == "start" && m_status.state != "Ready") ||
            (operation == "resume" && m_status.state != "Paused"))
            return false;
        if (m_preparedPlan["taskId"].toString() != m_status.taskId ||
            m_preparedPlan["revision"].toInt() != m_status.taskRevision)
            return false;
        body["motionPermit"] = m_permit;
        body["validForMs"] = int(qBound(qint64(1), m_permitDeadline - m_clock.elapsed(), qint64(3000)));
    }
    const bool sent = sendCommand("/control", body, operation,
                                  emergency || stop, requestId);
    if (sent && motion)
        m_permit.clear();
    return sent;
}
void TrackingClient::pollCommands() {
    const auto requests = m_pending.keys();
    for (const auto &id : requests) {
        const auto it = m_pending.constFind(id);
        if (it == m_pending.cend() || m_clock.elapsed() - it->createdAt <= 10000) continue;
        const auto pending = m_pending.take(id);
        if (!busy()) emit pendingChanged(false);
        emit commandUncertain(pending.operation, id);
    }
    pollCommandLane(true);
    pollCommandLane(false);
}
void TrackingClient::noteAccepted(const QString &id, const QString &commandId) {
    auto it = m_pending.find(id);
    if (it == m_pending.end()) return;
    if (!commandId.isEmpty()) it->commandId = commandId;
    if (!it->accepted) {
        it->accepted = true;
        emit commandAccepted(it->operation, id);
    }
}
void TrackingClient::pollCommandLane(bool safety) {
    auto &http = safety ? m_safetyHttp : m_http;
    auto &nextResult = safety ? m_nextSafetyResult : m_nextCommandResult;
    if (m_pending.isEmpty() || http.busy("result") || m_clock.elapsed() < nextResult)
        return;
    auto it = m_pending.end();
    // Round-robin by last query time prevents one unknown request starving later stops.
    for (auto candidate = m_pending.begin(); candidate != m_pending.end(); ++candidate)
        if (candidate->safety == safety && (safety || !candidate->postPending) &&
            m_clock.elapsed() >= candidate->nextQueryAt &&
            (it == m_pending.end() || candidate->queriedAt < it->queriedAt))
            it = candidate;
    if (it == m_pending.end()) return;
    const auto p = it.value();
    QUrlQuery query;
    QString path;
    if (p.commandId.isEmpty()) {
        path = "/commands";
        query.addQueryItem("requestId", p.requestId);
        query.addQueryItem("sessionId", p.sessionId);
    } else
        path = "/commands/" + p.commandId;
    const bool issued = http.request("result", "GET", path, {}, query, {}, [this, p](const JsonHttpResult &r) {
        if (!m_pending.contains(p.requestId))
            return;
        // The result lane belongs to this pending request. Never resolve it
        // using another request's reply, including after a delayed response.
        const bool matching = (!r.object.contains("requestId") ||
                               r.object["requestId"].toString() == p.requestId) &&
            (p.commandId.isEmpty() || !r.object.contains("commandId") ||
             r.object["commandId"].toString() == p.commandId);
        if (r.ok() && matching && QStringList{"applied", "rejected", "expired"}.contains(r.object["state"].toString())) {
            m_pending.remove(p.requestId);
            if (p.operation == "reset_fault" && r.object["state"].toString() == "applied") {
                invalidatePreparedPlan();
                revokeMotionPermit();
                m_nextHealth = 0;
                refreshConfiguration();
            }
            if (!busy()) emit pendingChanged(false);
            auto result = r.object;
            result["requestId"] = p.requestId;
            emit commandFinished(p.operation, result);
            return;
        }
        if (r.ok() && matching && !r.object["commandId"].toString().isEmpty())
            noteAccepted(p.requestId, r.object["commandId"].toString());
        // Ordinary unresolved results back off after the first queries. Safety
        // confirmation keeps its own 100 ms cadence, independent of slow writes.
        auto it = m_pending.find(p.requestId);
        if (it != m_pending.end())
            it->nextQueryAt = m_clock.elapsed() + (p.safety ? 100 : it->queryCount < 2 ? 200 : 500);
    });
    if (issued && m_pending.contains(p.requestId)) {
        m_pending[p.requestId].queriedAt = m_clock.elapsed();
        ++m_pending[p.requestId].queryCount;
        nextResult = m_clock.elapsed() + 100;
    }
}
void TrackingClient::pollEvents() {
    // Status and the event cursor belong to the same boot. A new event is
    // advertised by status; querying an already consumed cursor only returns
    // an empty response. Keep the cursor unchanged on errors and partial pages.
    if (m_historyContextBoot != m_status.bootId || m_historyContextTask != m_status.taskId ||
        m_historyContextExecution != m_status.executionId || m_historyContextRevision != m_status.taskRevision) {
        m_historyContextBoot = m_status.bootId; m_historyContextTask = m_status.taskId;
        m_historyContextExecution = m_status.executionId; m_historyContextRevision = m_status.taskRevision;
        m_historyAttempts = 0;
    }
    if (m_historyAttempts >= 3 && !m_historyRead) return;
    if (m_historyRead) {
        if (m_historyRead->boot != m_status.bootId || m_historyRead->execution != m_status.executionId ||
            m_historyRead->task != m_status.taskId || m_historyRead->taskRevision != m_status.taskRevision)
            m_historyRead.reset();
        else {
            if (commandFresh() && m_clock.elapsed() >= m_nextEvents) pollExecutionHistory();
            return;
        }
    }
    if (!commandFresh() || (m_status.hasEventSequence && m_status.lastEventSeq <= m_eventCursor) ||
        m_clock.elapsed() < m_nextEvents)
        return;
    m_nextEvents = m_clock.elapsed() + 200;
    QUrlQuery q;
    q.addQueryItem("after", QString::number(m_eventCursor));
    q.addQueryItem("waitMs", "100");
    const auto boot = m_status.bootId;
    const auto generation = m_metadataGeneration;
    const auto execution = m_status.executionId;
    const auto task = m_status.taskId;
    const auto taskRevision = m_status.taskRevision;
    const auto resumeCursor = m_status.lastEventSeq;
    m_http.request("events", "GET", "/events", {}, q, {}, [this, boot, generation, execution, task, taskRevision, resumeCursor](const JsonHttpResult &r) {
        if (generation != m_metadataGeneration || boot != m_status.bootId)
            return;
        if (r.status == 410) {
            if (execution != m_status.executionId || task != m_status.taskId || taskRevision != m_status.taskRevision) return;
            if (execution.isEmpty()) {
                m_eventCursor = resumeCursor;
                emit eventsResynchronized();
            } else {
                HistoryRead read;
                read.boot = boot; read.execution = execution; read.task = task;
                read.taskRevision = taskRevision;
                read.generation = ++m_historyGeneration; read.resumeCursor = resumeCursor;
                m_historyRead = std::move(read);
                ++m_historyAttempts;
                m_nextEvents = m_clock.elapsed();
            }
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
void TrackingClient::pollExecutionHistory() {
    if (!m_historyRead || m_http.busy("execution")) return;
    const auto read = *m_historyRead;
    QUrlQuery query;
    query.addQueryItem("limit", "100");
    if (!read.cursor.isEmpty()) query.addQueryItem("cursor", read.cursor);
    m_nextEvents = m_clock.elapsed() + 200;
    m_http.request("execution", "GET", "/executions/" + read.execution, {}, query, {},
        [this, read](const JsonHttpResult &result) {
            if (!m_historyRead || m_historyRead->generation != read.generation ||
                read.boot != m_status.bootId || read.execution != m_status.executionId ||
                read.task != m_status.taskId || read.taskRevision != m_status.taskRevision) return;
            const auto unavailable = [this] {
                m_historyRead.reset();
                m_nextEvents = m_clock.elapsed() + 5000;
                emit errorOccurred(m_historyAttempts >= 3 ? QStringLiteral("execution_history_recovery_exhausted") :
                    QStringLiteral("execution_history_unavailable"));
            };
            const auto &object = result.object;
            const bool paged = object.contains("hasMore");
            const auto events = object.value("events").toArray();
            quint64 highWater = 0;
            if (!result.ok() || read.pages >= 1024 || object.value("apiVersion").toInt() != 1 ||
                object.value("executionId").toString() != read.execution || !object.value("events").isArray() ||
                (paged && (!object.value("hasMore").isBool() || events.size() > 100 ||
                    object.value("historyBootId").toString() != read.boot ||
                    !TrackingJson::sequence(object.value("historyLastSeq"), &highWater) ||
                    (read.highWaterKnown && highWater != read.highWater)))) { unavailable(); return; }
            quint64 lastSeq = read.lastSeq;
            for (const auto &value : events) {
                quint64 seq = 0;
                if (!value.isObject() || !TrackingJson::sequence(value.toObject().value("seq"), &seq) ||
                    seq <= lastSeq || (paged && seq > highWater)) { unavailable(); return; }
                lastSeq = seq;
            }
            const bool more = object.value("hasMore").toBool();
            const auto cursor = object.value("nextCursor").toString();
            const auto cursorHash = QCryptographicHash::hash(cursor.toUtf8(), QCryptographicHash::Sha256);
            if (more && (events.isEmpty() || cursor.isEmpty() || cursor.size() > 2048 ||
                read.seenCursors.contains(cursorHash) || lastSeq >= highWater)) {
                unavailable(); return;
            }
            if (paged && !more && lastSeq != highWater) { unavailable(); return; }
            // Stream one bounded page to observers, never accumulate history.
            // Only a fully read snapshot advances the live-event cursor.
            if (more) {
                m_historyRead->cursor = cursor;
                m_historyRead->lastSeq = lastSeq;
                m_historyRead->highWater = highWater;
                m_historyRead->highWaterKnown = true;
                m_historyRead->seenCursors.insert(cursorHash);
                ++m_historyRead->pages;
            } else {
                m_historyRead.reset();
                m_historyAttempts = 0;
                m_eventCursor = read.resumeCursor;
            }
            const QPointer<TrackingClient> guard(this);
            const auto generation = m_metadataGeneration;
            emit executionReceived(object);
            if (guard && generation == m_metadataGeneration && !more) emit eventsResynchronized();
        });
}
bool TrackingClient::acknowledge(const QJsonObject &event, bool success) {
    const auto id = event["eventId"].toString();
    if (!commandFresh() || id.isEmpty() || id != m_status.waitingEventId ||
        event["executionId"].toString() != m_status.executionId ||
        event["stepId"].toString() != m_status.stepId)
        return false;
    return sendCommand(
        "/events/" + id + "/ack",
        {{"executionId", m_status.executionId}, {"stepId", m_status.stepId}, {"success", success}}, "ack");
}
bool TrackingClient::updateOrigin(const QJsonObject &origin, const QString &revision) {
    if (!commandFresh() || !hasSession() || busy())
        return false;
    return m_http.request("origin", "POST", "/origin",
                          {{"requestId", TrackingJson::newId()},
                           {"sessionId", m_sessionId},
                           {"expectedRevision", revision},
                           {"origin", origin}},
                          {}, m_sessionToken, [this](const JsonHttpResult &r) {
                              if (r.ok()) {
                                  refreshConfiguration();
                                  emit originChanged(r.object);
                              } else
                                  emit errorOccurred(r.error);
                          });
}
bool TrackingClient::readTask(const QString &id, int revision) {
    m_readTask = {};
    if (!commandFresh() || id != m_status.taskId || revision != m_status.taskRevision)
        return false;
    const auto boot = m_status.bootId;
    const auto execution = m_status.executionId;
    QUrlQuery query;
    query.addQueryItem("revision", QString::number(revision));
    return m_http.request("task", "GET", "/tasks/" + id, {}, query, {}, [this, id, revision, boot, execution](const JsonHttpResult &r) {
        if (!commandFresh() || boot != m_status.bootId || id != m_status.taskId ||
            revision != m_status.taskRevision || execution != m_status.executionId)
            return;
        const auto plan = r.object["plan"].toObject();
        static const QRegularExpression hashPattern("^fnv1a64:[0-9a-f]{16}$");
        if (r.ok() && r.object["apiVersion"].toInt() == 1 &&
            r.object["taskId"].toString() == id && r.object["revision"].toInt() == revision &&
            plan["taskId"].toString() == id && plan["revision"].toInt() == revision &&
            TrackingContext::fromJson(plan["context"].toObject()).isComplete() &&
            !plan["steps"].toArray().isEmpty() && hashPattern.match(r.object["planHash"].toString()).hasMatch()) {
            m_readTask = r.object;
            m_readTaskBoot = boot;
            m_readTaskExecution = execution;
            emit taskReceived(m_readTask);
        } else
            emit errorOccurred(r.error.isEmpty() ? "invalid_task_readback" : r.error);
    }, 16 * 1024 * 1024);
}
bool TrackingClient::confirmReadTask(const QJsonObject &record, const TrackingContext &context) {
    const auto plan = record["plan"].toObject();
    if (record.isEmpty() || record != m_readTask || !commandFresh() || !configurationFresh() || !hasSession() || busy() ||
        m_readTaskBoot != m_status.bootId || m_readTaskExecution != m_status.executionId ||
        record["taskId"].toString() != m_status.taskId || record["revision"].toInt() != m_status.taskRevision ||
        !context.isComplete() || plan["context"].toObject() != context.toJson())
        return false;
    m_preparedPlan = plan;
    return true;
}
