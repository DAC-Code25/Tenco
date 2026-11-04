#include "chassisclient.h"
#include "controlsessioncoordinator.h"
#include "externaleventcoordinator.h"
#include "jsonhttpclient.h"
#include "poseclient.h"
#include "trackingclient.h"
#include <QJsonArray>
#include <QDateTime>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QWebSocket>
#include <QWebSocketServer>
#include <QtTest>

class HttpFixture : public QTcpServer {
  public:
    std::function<void(QTcpSocket *, const QByteArray &, const QJsonObject &)> handler;
    HttpFixture() {
        listen(QHostAddress::LocalHost);
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto *socket = nextPendingConnection();
                auto data = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, data] {
                    data->append(socket->readAll());
                    const int end = data->indexOf("\r\n\r\n");
                    if (end < 0 || socket->property("handled").toBool())
                        return;
                    const auto header = data->left(end);
                    int length = 0;
                    for (const auto &line : header.split('\n'))
                        if (line.toLower().startsWith("content-length:"))
                            length = line.mid(15).trimmed().toInt();
                    if (data->size() < end + 4 + length)
                        return;
                    socket->setProperty("handled", true);
                    socket->setProperty("requestHeaders", header);
                    handler(socket, header.split(' ').value(1),
                            QJsonDocument::fromJson(data->mid(end + 4, length)).object());
                });
            }
        });
    }
    QUrl url() const { return QUrl(QString("http://127.0.0.1:%1/api/v1").arg(serverPort())); }
    static QByteArray requestHeader(QTcpSocket *socket, const QByteArray &name) {
        for (const auto &line : socket->property("requestHeaders").toByteArray().split('\n')) {
            const auto colon = line.indexOf(':');
            if (colon >= 0 && line.left(colon).trimmed().compare(name, Qt::CaseInsensitive) == 0)
                return line.mid(colon + 1).trimmed();
        }
        return {};
    }
    static void reply(QTcpSocket *socket, const QJsonObject &json, int code = 200) {
        const auto bytes = QJsonDocument(json).toJson(QJsonDocument::Compact);
        socket->write("HTTP/1.1 " + QByteArray::number(code) +
                      " Result\r\nContent-Type: application/json\r\nContent-Length: " +
                      QByteArray::number(bytes.size()) + "\r\nConnection: close\r\n\r\n" + bytes);
        socket->disconnectFromHost();
    }
    static void streamHeaders(QTcpSocket *socket) {
        socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/x-ndjson\r\n"
                      "Transfer-Encoding: chunked\r\nCache-Control: no-store\r\n\r\n");
    }
    static void chunk(QTcpSocket *socket, const QByteArray &data) {
        socket->write(QByteArray::number(data.size(), 16) + "\r\n" + data + "\r\n");
    }
    static QTimer *stream(QTcpSocket *socket, std::function<QJsonObject()> next, int intervalMs = 20) {
        streamHeaders(socket);
        auto *timer = new QTimer(socket);
        timer->setTimerType(Qt::PreciseTimer);
        QObject::connect(timer, &QTimer::timeout, socket, [socket, next] {
            const auto object = next();
            if (!object.isEmpty())
                chunk(socket, QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n');
        });
        timer->start(intervalMs);
        return timer;
    }
};

static QJsonObject poseObject(const QString &seq = "1", const QString &boot = "pose-a") {
    return {{"apiVersion", 1},
            {"seq", seq},
            {"bootId", boot},
            {"frameId", "map"},
            {"baseFrameId", "base_link"},
            {"originRevision", "origin"},
            {"calibrationId", "cal"},
            {"clockDomain", "system"},
            {"localizationMode", "nominal"},
            {"stateAgeMs", 0},
            {"sendTime", QDateTime::currentMSecsSinceEpoch() / 1000.0},
            {"positionObservationAgeMs", 0},
            {"headingObservationAgeMs", 0},
            {"pose", QJsonObject{{"x", 0}, {"y", 0}, {"yaw", 0}}},
            {"estimatedTwist", QJsonObject{{"v", 0}, {"omega", 0}}},
            {"validForControl", true},
            {"positionReliable", true},
            {"headingReliable", true}};
}
static QJsonObject statusObject(quint64 seq, const QString &boot) {
    QJsonObject value{{"apiVersion", 1},
                      {"seq", QString::number(seq)},
                      {"bootId", boot},
                      {"stateVersion", "1"},
                      {"lastEventSeq", "0"},
                      {"state", "Ready"},
                      {"taskId", "task"},
                      {"taskRevision", 1},
                      {"measuredTwist", QJsonObject{{"v", 0}, {"omega", 0}}}};
    for (const auto *key : {"stepProgressMeters", "stepLengthMeters", "lateralErrorMeters", "headingErrorRad",
                            "remainingAngleRad", "remainingWaitMs"})
        value[key] = 0;
    return value;
}

// Separate persistence completion, status visibility and POST responses to model
// wireless delays without sending any real chassis command.
class DeploymentFixture : public HttpFixture {
  public:
    QString boot = "deploy-boot", state = "Idle", task, execution;
    quint64 seq = 0, version = 1;
    QJsonObject plan;
    QString prepareRequest, startRequest;
    int uploads = 0, starts = 0, startQueries = 0;
    bool offline = false, prepareApplied = false, lostResponses = false;
    bool omitRequestId = false, mismatchResult = false, rejectPrepare = false;
    DeploymentFixture() {
        handler = [this](auto *socket, const QByteArray &path, const QJsonObject &body) {
            if (path.startsWith("/api/v1/status")) {
                auto status = statusObject(++seq, boot);
                status["state"] = state; status["taskId"] = task;
                status["executionId"] = execution;
                status["stateVersion"] = QString::number(version);
                reply(socket, status, offline ? 503 : 200);
            } else if (path.endsWith("/configuration"))
                reply(socket, {{"apiVersion", 1}, {"profile_revision", "profile"},
                    {"calibration", QJsonObject{{"calibrationId", "cal"}}},
                    {"origin", QJsonObject{{"active", QJsonObject{{"originRevision", "origin"}}}}},
                    {"navigation_defaults", QJsonObject{{"rotation_policy", "vehicle_footprint"}}}});
            else if (path.endsWith("/health"))
                reply(socket, {{"apiVersion", 1}, {"bootId", boot}, {"ready", true}});
            else if (path.endsWith("/sessions"))
                reply(socket, {{"apiVersion", 1}, {"bootId", boot}, {"sessionId", "session"},
                    {"sessionToken", "token"}, {"expiresInMs", 1000}});
            else if (path.endsWith("/heartbeat"))
                reply(socket, {{"apiVersion", 1}, {"bootId", boot}, {"sessionId", "session"},
                    {"stateVersion", QString::number(version)}, {"motionPermit", "permit"},
                    {"permitValidForMs", 500}});
            else if (path.endsWith("/tasks")) {
                ++uploads; plan = body; prepareRequest = body["requestId"].toString();
                reply(socket, lostResponses ? QJsonObject{} : QJsonObject{{"commandId", "prepare"}},
                      lostResponses ? 503 : 202);
            } else if (path.endsWith("/control")) {
                const auto op = body["cmd"].toString();
                if (op == "start") {
                    ++starts; startRequest = body["requestId"].toString();
                    state = "Tracking"; execution = "execution"; ++version;
                }
                reply(socket, lostResponses ? QJsonObject{} : QJsonObject{{"commandId", op}},
                      lostResponses ? 503 : 202);
            } else if (path.startsWith("/api/v1/commands")) {
                const QUrlQuery query(QUrl(QString::fromUtf8(path)));
                const bool prepare = path.endsWith("/prepare") ||
                    (!prepareRequest.isEmpty() && query.queryItemValue("requestId") == prepareRequest);
                const bool start = path.endsWith("/start") ||
                    (!startRequest.isEmpty() && query.queryItemValue("requestId") == startRequest);
                // Query and POST lanes are independent. A lookup may arrive
                // before the POST has been accepted; it must not invent a result.
                if (!prepare && !start) { reply(socket, {}, 404); return; }
                if (start) ++startQueries;
                const auto id = prepare ? prepareRequest : startRequest;
                QJsonObject result{{"commandId", prepare ? "prepare" : "start"},
                    {"state", prepare && rejectPrepare ? "rejected" : prepare && !prepareApplied ? "accepted" : "applied"}};
                if (!omitRequestId) result["requestId"] = mismatchResult ? "unrelated-request" : id;
                reply(socket, result);
            } else reply(socket, {{"apiVersion", 1}, {"bootId", boot}, {"events", QJsonArray{}}});
        };
    }
    void showReady() {
        task = plan["taskId"].toString(); state = "Ready"; execution.clear(); ++version;
    }
};

class IpcClientsTest : public QObject {
    Q_OBJECT
  private slots:
    void operatorMessagesTranslateInternalReasons() {
        TrackingClient tracking;
        PoseClient pose;
        ControlSessionCoordinator coordinator(&tracking, &pose);
        QSignalSpy rawErrors(&pose, &PoseClient::errorOccurred);
        QSignalSpy messages(&coordinator, &ControlSessionCoordinator::message);
        emit pose.errorOccurred(QStringLiteral("pose_stream_disconnected"));
        QCOMPARE(rawErrors.last().first().toString(), QStringLiteral("pose_stream_disconnected"));
        QCOMPARE(messages.last().first().toString(), QStringLiteral("定位连接已断开，正在重连"));
        emit tracking.errorOccurred(QStringLiteral("request_timeout"));
        QCOMPARE(messages.last().first().toString(), QStringLiteral("工控机响应超时，请稍后重试"));
        emit tracking.errorOccurred(QStringLiteral("future_navigation_error"));
        QCOMPARE(messages.last().first().toString(), QStringLiteral("操作未完成，请查看维护诊断"));
        emit tracking.commandFinished("pause", {{"state", "applied"}});
        QVERIFY(messages.last().first().toString().contains(QStringLiteral("暂停任务：已应用")));
        QVERIFY(!messages.last().first().toString().contains(QStringLiteral("applied")));
        QCOMPARE(TrackingJson::reasonText(QStringLiteral("pose_stream_http_401")),
                 QStringLiteral("工控机连接验证失败，请检查连接设置"));
        QCOMPARE(TrackingJson::reasonText(QStringLiteral("pose_stream_http_503")),
                 QStringLiteral("工控机服务暂不可用，请检查服务状态"));
        QCOMPARE(TrackingJson::reasonText(QStringLiteral("请等待车辆停稳")), QStringLiteral("请等待车辆停稳"));
        QVERIFY(TrackingJson::reasonText({}).isEmpty());
    }
    void healthFreshnessUsesIndependentAgeAndRejectsPreviousBootOrConfiguration() {
        DeploymentFixture server, replacement;
        replacement.boot = "replacement-boot";
        const auto original = server.handler;
        bool holdHealth = false;
        QPointer<QTcpSocket> held;
        QString heldBoot;
        server.handler = [&](auto *socket, const QByteArray &path, const QJsonObject &body) {
            if (path.endsWith("/health")) {
                if (holdHealth) { held = socket; heldBoot = server.boot; return; }
                const auto boot = server.boot;
                QTimer::singleShot(150, socket, [socket, boot] {
                    HttpFixture::reply(socket, {{"apiVersion", 1}, {"bootId", boot}, {"ready", true}});
                });
            } else original(socket, path, body);
        };
        TrackingClient client;
        client.configure(server.url(), {}, 5000);
        QCOMPARE(client.healthAgeMs(), qint64(-1));
        client.start();
        QTRY_VERIFY(client.healthFresh());
        QVERIFY(client.healthAgeMs() >= 130);
        const auto initialAge = client.healthAgeMs();
        QTest::qWait(100);
        QVERIFY(client.healthAgeMs() >= initialAge + 80);
        holdHealth = true;
        QTRY_VERIFY(held);
        QTRY_VERIFY_WITH_TIMEOUT(!client.healthFresh(), 2300);
        QVERIFY(client.fresh()); // New status replies cannot refresh health age.
        QVERIFY(client.healthAgeMs() >= 2000);
        server.boot = "restarted-boot";
        QTRY_COMPARE(client.snapshot().bootId, server.boot);
        QCOMPARE(client.healthAgeMs(), qint64(-1));
        holdHealth = false;
        HttpFixture::reply(held, {{"apiVersion", 1}, {"bootId", heldBoot}, {"ready", true}});
        held.clear();
        QTRY_VERIFY(client.healthFresh());
        QCOMPARE(client.health().value("bootId").toString(), server.boot);
        holdHealth = true;
        QTRY_VERIFY(held);
        client.configure(replacement.url(), {}, 5000);
        QCOMPARE(client.healthAgeMs(), qint64(-1));
        if (held && held->state() == QAbstractSocket::ConnectedState)
            HttpFixture::reply(held, {{"apiVersion", 1}, {"bootId", server.boot}, {"ready", true}});
        QTRY_VERIFY(client.healthFresh());
        QCOMPARE(client.health().value("bootId").toString(), replacement.boot);
        client.stop();
        QCOMPARE(client.healthAgeMs(), qint64(-1));
        QVERIFY(!client.healthFresh());
    }
    void statusPollingAdaptsToRoundTripWithoutOverlapping_data() {
        QTest::addColumn<int>("responseDelayMs");
        QTest::newRow("fast-link") << 0;
        QTest::newRow("moderate-link") << 240;
        QTest::newRow("slow-link-capped") << 700;
    }
    void statusPollingAdaptsToRoundTripWithoutOverlapping() {
        QFETCH(int, responseDelayMs);
        DeploymentFixture server; const auto original = server.handler;
        QElapsedTimer clock; clock.start();
        QList<qint64> requests;
        int active = 0, maximumActive = 0;
        server.handler = [&](auto *socket, const QByteArray &path, const QJsonObject &body) {
            if (path.startsWith("/api/v1/status")) {
                requests.append(clock.elapsed());
                maximumActive = qMax(maximumActive, ++active);
                QTimer::singleShot(responseDelayMs, socket, [&, socket, path, body] {
                    --active; original(socket, path, body);
                });
            } else original(socket, path, body);
        };
        TrackingClient tracking; tracking.configure(server.url(), {}, 3000); tracking.start();
        QTRY_VERIFY_WITH_TIMEOUT(requests.size() >= 4, 7000);
        QCOMPARE(maximumActive, 1);
        const qint64 expectedGap = responseDelayMs + qBound(100, responseDelayMs, 500);
        for (int index = 1; index < 4; ++index) {
            const auto gap = requests[index] - requests[index - 1];
            QVERIFY2(gap >= expectedGap - 60, qPrintable(QString("poll gap %1 < %2").arg(gap).arg(expectedGap - 60)));
            QVERIFY2(gap <= expectedGap + 300, qPrintable(QString("poll gap %1 > %2").arg(gap).arg(expectedGap + 300)));
        }
        tracking.stop();
    }
    void configurationRefreshIsSeparateFromHealthAndGatesCachedConfiguration() {
        DeploymentFixture server; const auto original = server.handler;
        int healthRequests = 0, configurationRequests = 0;
        bool holdConfiguration = false;
        QPointer<QTcpSocket> held;
        server.handler = [&](auto *socket, const QByteArray &path, const QJsonObject &body) {
            if (path.endsWith("/health")) ++healthRequests;
            if (path.endsWith("/configuration")) {
                ++configurationRequests;
                if (holdConfiguration) { held = socket; return; }
            }
            original(socket, path, body);
        };
        TrackingClient tracking; tracking.configure(server.url(), {}, 5000); tracking.start();
        QTRY_VERIFY(tracking.configurationFresh());
        const auto cached = tracking.configuration();
        tracking.acquireSession(); QTRY_VERIFY(tracking.motionReady());
        QTest::qWait(3200);
        QCOMPARE(configurationRequests, 1);
        QVERIFY(healthRequests >= 3);
        QTRY_COMPARE_WITH_TIMEOUT(configurationRequests, 2, 2500);
        QTRY_VERIFY(tracking.configurationFresh());
        holdConfiguration = true;
        tracking.refreshConfiguration();
        QVERIFY(!tracking.configurationFresh());
        QCOMPARE(tracking.configuration(), cached); // Display keeps the known snapshot.
        QVERIFY(!tracking.motionReady());
        QVERIFY(!tracking.upload({{"taskId", "task"}, {"revision", 1}}));
        QTRY_VERIFY(held);
        tracking.refreshConfiguration(); // This in-flight response is now obsolete.
        holdConfiguration = false;
        HttpFixture::reply(held, {{"apiVersion", 1}, {"bootId", server.boot}, {"marker", "obsolete"}});
        QTRY_VERIFY(tracking.configurationFresh());
        QCOMPARE(configurationRequests, 4);
        QCOMPARE(tracking.configuration(), cached);
        QTRY_VERIFY(tracking.motionReady());
        QVERIFY(tracking.releaseSessionForMaintenance());
        QVERIFY(!tracking.configurationFresh());
        QTRY_COMPARE(configurationRequests, 5);
        QTRY_VERIFY(tracking.configurationFresh());
        tracking.acquireSession(); QTRY_VERIFY(tracking.motionReady());
        held.clear(); holdConfiguration = true;
        QTRY_VERIFY_WITH_TIMEOUT(held, 6000); // The normal five-second refresh stalls.
        QTest::qWait(1200);
        QVERIFY(tracking.commandFresh()); QVERIFY(tracking.hasSession());
        QVERIFY(!tracking.configurationFresh());
        QCOMPARE(tracking.configuration(), cached);
        QVERIFY(!tracking.upload({{"taskId", "task"}, {"revision", 1}}));
        QVERIFY(!tracking.motionReady());
        tracking.stop();
    }
    void slowWriteDoesNotFloodResultsOrDelaySafetyConfirmation() {
        DeploymentFixture server; const auto original = server.handler;
        QPointer<QTcpSocket> uploadReply;
        QElapsedTimer clock; clock.start();
        QList<qint64> ordinaryQueries;
        int safetyQueries = 0, safetyWrites = 0;
        server.handler = [&](auto *socket, const QByteArray &path, const QJsonObject &body) {
            if (path.endsWith("/tasks")) {
                ++server.uploads; server.prepareRequest = body["requestId"].toString();
                uploadReply = socket;
            } else if (path.startsWith("/api/v1/commands")) {
                const QUrlQuery query(QUrl(QString::fromUtf8(path)));
                const bool safety = path.endsWith("/estop") || query.queryItemValue("sessionId") == "emergency";
                if (safety) {
                    ++safetyQueries;
                    HttpFixture::reply(socket, {{"commandId", "estop"}, {"state", "applied"}});
                } else {
                    ordinaryQueries.append(clock.elapsed());
                    HttpFixture::reply(socket, {{"commandId", "prepare"}, {"requestId", server.prepareRequest},
                        {"state", ordinaryQueries.size() < 3 ? "accepted" : "applied"}});
                }
            } else {
                if (path.endsWith("/control") && body["cmd"] == "estop") ++safetyWrites;
                original(socket, path, body);
            }
        };
        TrackingClient tracking; tracking.configure(server.url(), {}, 5000); tracking.start();
        QTRY_VERIFY(tracking.configurationFresh());
        tracking.acquireSession(); QTRY_VERIFY(tracking.hasSession());
        QVERIFY(tracking.upload({{"taskId", "task"}, {"revision", 1}}));
        QTRY_VERIFY(uploadReply);
        QTest::qWait(350);
        QCOMPARE(ordinaryQueries.size(), 0);
        QSignalSpy finished(&tracking, &TrackingClient::commandFinished);
        QVERIFY(tracking.control("estop"));
        QTRY_COMPARE_WITH_TIMEOUT(safetyQueries, 1, 750);
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(finished.first().first().toString(), QString("estop"));
        QCOMPARE(ordinaryQueries.size(), 0);
        HttpFixture::reply(uploadReply, {{"commandId", "prepare"}}, 202);
        QTRY_COMPARE_WITH_TIMEOUT(ordinaryQueries.size(), 3, 2500);
        QTRY_VERIFY(!tracking.busy());
        QVERIFY(ordinaryQueries[1] - ordinaryQueries[0] >= 150);
        QVERIFY(ordinaryQueries[2] - ordinaryQueries[1] >= 450);
        QCOMPARE(server.uploads, 1); QCOMPARE(safetyWrites, 1);
        QTest::qWait(300);
        QCOMPARE(ordinaryQueries.size(), 3); QCOMPARE(safetyQueries, 1);
        tracking.stop();
    }
    void pendingNotificationDoesNotWaitForAnotherMonitorSnapshot() {
        HttpFixture server;
        quint64 seq = 0;
        int writes = 0;
        bool monitor = true, matching = false, applied = false;
        QString request;
        server.handler = [&](auto *socket, const QByteArray &path, const QJsonObject &body) {
            if (path.startsWith("/api/v1/status"))
                HttpFixture::reply(socket, monitor ? statusObject(++seq, "boot") : QJsonObject{}, monitor ? 200 : 204);
            else if (path.endsWith("/sessions"))
                HttpFixture::reply(socket, {{"apiVersion", 1}, {"bootId", "boot"}, {"sessionId", "session"},
                    {"sessionToken", "token"}, {"expiresInMs", 10000}});
            else if (path.endsWith("/heartbeat"))
                HttpFixture::reply(socket, {{"apiVersion", 1}, {"bootId", "boot"}, {"sessionId", "session"},
                    {"stateVersion", "1"}, {"motionPermit", "permit"}, {"permitValidForMs", 3000}});
            else if (path.endsWith("/tasks")) {
                ++writes; request = body["requestId"].toString();
                HttpFixture::reply(socket, {{"commandId", "prepare"}}, 202);
            } else if (path.startsWith("/api/v1/commands"))
                HttpFixture::reply(socket, {{"commandId", "prepare"},
                    {"requestId", matching ? request : QString("another-request")},
                    {"state", applied ? "applied" : "accepted"}});
            else HttpFixture::reply(socket, {{"apiVersion", 1}, {"bootId", "boot"}, {"ready", true}, {"events", QJsonArray{}}});
        };
        TrackingClient tracking;
        tracking.configure(server.url(), {}, 500); tracking.start();
        QTRY_VERIFY(tracking.fresh()); tracking.acquireSession(); QTRY_VERIFY(tracking.hasSession());
        QTRY_VERIFY(tracking.configurationFresh());
        QSignalSpy pending(&tracking, &TrackingClient::pendingChanged);
        QVERIFY(tracking.upload({{"taskId", "task"}, {"revision", 1}}));
        QCOMPARE(pending.size(), 1); QVERIFY(pending.first().first().toBool());
        QTRY_COMPARE(writes, 1);
        monitor = false;
        QTest::qWait(100); // Drain an already issued status response.
        const auto lastSeq = tracking.snapshot().seq;
        applied = true; // Another request's applied result cannot unlock motion.
        QTest::qWait(150);
        QVERIFY(tracking.busy()); QCOMPARE(pending.size(), 1);
        matching = true;
        QTRY_VERIFY(!tracking.busy());
        QCOMPARE(pending.size(), 2); QVERIFY(!pending.last().first().toBool());
        QCOMPARE(tracking.snapshot().seq, lastSeq); QCOMPARE(writes, 1);
        tracking.stop(); QCOMPARE(pending.size(), 2);
    }
    void faultResetRequiresMeasuredStopAndClearsPlanOnlyWhenApplied_data() {
        QTest::addColumn<bool>("applyReset");
        QTest::newRow("applied") << true;
        QTest::newRow("rejected") << false;
    }
    void faultResetRequiresMeasuredStopAndClearsPlanOnlyWhenApplied() {
        QFETCH(bool, applyReset);
        HttpFixture server;
        quint64 seq = 0, version = 1;
        QString state = "Ready", stopRequest, resetRequest, prepareRequest;
        bool valid = false, stopped = false, connected = true, stopApplied = false, resetFinished = false;
        int resets = 0, starts = 0, sessions = 0;
        server.handler = [&](auto *socket, const QByteArray &path, const QJsonObject &body) {
            if (path.startsWith("/api/v1/status")) {
                auto status = statusObject(++seq, "boot");
                status["state"] = state; status["stateVersion"] = QString::number(version);
                status["executionId"] = state == "Fault" ? "old-execution" : "";
                status["chassisConnected"] = connected; status["measuredStopped"] = stopped;
                status["measuredTwist"] = QJsonObject{{"v", 0}, {"omega", 0}, {"valid", valid}};
                HttpFixture::reply(socket, status);
            } else if (path.endsWith("/configuration"))
                HttpFixture::reply(socket, {{"apiVersion", 1}, {"profile_revision", "profile"},
                    {"calibration", QJsonObject{{"calibrationId", "cal"}}},
                    {"origin", QJsonObject{{"active", QJsonObject{{"originRevision", "origin"}}}}},
                    {"navigation_defaults", QJsonObject{{"rotation_policy", "vehicle_footprint"}}}});
            else if (path.endsWith("/sessions")) {
                ++sessions;
                HttpFixture::reply(socket, {{"apiVersion", 1}, {"bootId", "boot"}, {"sessionId", "session"},
                    {"sessionToken", "token"}, {"expiresInMs", 10000}});
            } else if (path.endsWith("/heartbeat"))
                HttpFixture::reply(socket, {{"apiVersion", 1}, {"bootId", "boot"}, {"sessionId", "session"},
                    {"stateVersion", QString::number(version)}, {"motionPermit", "permit"}, {"permitValidForMs", 3000}});
            else if (path.endsWith("/tasks")) {
                prepareRequest = body["requestId"].toString();
                HttpFixture::reply(socket, {{"commandId", "prepare"}}, 202);
            } else if (path.endsWith("/control")) {
                const auto op = body["cmd"].toString();
                if (op == "reset_fault") {
                    ++resets; resetRequest = body["requestId"].toString();
                    QCOMPARE(body["expectedStateVersion"].toString(), QString::number(version));
                    QVERIFY(!body.contains("motionPermit"));
                } else if (op == "estop") stopRequest = body["requestId"].toString();
                else if (op == "start" || op == "resume") ++starts;
                HttpFixture::reply(socket, {{"commandId", op}}, 202);
            } else if (path.startsWith("/api/v1/commands")) {
                const QUrlQuery query(QUrl(QString::fromUtf8(path)));
                const auto request = query.queryItemValue("requestId");
                QString id, commandState = "accepted";
                if (path.endsWith("/prepare") || (!prepareRequest.isEmpty() && request == prepareRequest)) {
                    id = prepareRequest; commandState = "applied";
                } else if (path.endsWith("/estop") || (!stopRequest.isEmpty() && request == stopRequest)) {
                    id = stopRequest; commandState = stopApplied ? "applied" : "accepted";
                } else if (path.endsWith("/reset_fault") || (!resetRequest.isEmpty() && request == resetRequest)) {
                    id = resetRequest;
                    if (resetFinished) commandState = applyReset ? "applied" : "rejected";
                }
                if (id.isEmpty()) HttpFixture::reply(socket, {}, 404);
                else HttpFixture::reply(socket, {{"requestId", id}, {"state", commandState}, {"reason", "fault_not_cleared"}});
            } else HttpFixture::reply(socket, {{"apiVersion", 1}, {"bootId", "boot"}, {"ready", true}, {"events", QJsonArray{}}});
        };
        TrackingClient tracking; PoseClient pose; ControlSessionCoordinator coordinator(&tracking, &pose);
        MapFrameBinding binding; binding.confirmed = true;
        binding.context = {"map", "origin", "transform", "cal", "profile", 1};
        coordinator.setBinding(binding);
        QSignalSpy messages(&coordinator, &ControlSessionCoordinator::message);
        coordinator.resetFault(); QVERIFY(messages.last().first().toString().contains("请先连接"));
        tracking.configure(server.url(), {}, 500); tracking.start();
        QTRY_VERIFY(coordinator.coordinatesReady());
        coordinator.resetFault(); QVERIFY(messages.last().first().toString().contains("没有需要复位"));
        const QJsonObject plan{{"taskId", "task"}, {"revision", 1}, {"context", binding.context.toJson()}};
        QVERIFY(coordinator.upload(plan));
        QTRY_VERIFY(!coordinator.actionPending() && !tracking.busy());
        QCOMPARE(tracking.preparedPlan(), plan);
        state = "Fault"; ++version;
        QTRY_COMPARE(tracking.snapshot().state, QString("Fault"));
        coordinator.resetFault(); QVERIFY(messages.last().first().toString().contains("实测已停稳"));
        QCOMPARE(resets, 0);
        valid = true; stopped = true; connected = false;
        QTRY_VERIFY(tracking.snapshot().measuredValid && tracking.snapshot().measuredStopped);
        QVERIFY(!tracking.canResetFault());
        connected = true; QTRY_VERIFY(tracking.canResetFault());
        QVERIFY(tracking.control("estop")); QTRY_VERIFY(!stopRequest.isEmpty());
        coordinator.resetFault(); QVERIFY(messages.last().first().toString().contains("结果待确认"));
        QCOMPARE(resets, 0);
        stopApplied = true; QTRY_VERIFY(!tracking.busy());
        QString releaseMessage; QVERIFY(tracking.releaseSessionForMaintenance(&releaseMessage));
        QVERIFY(!tracking.hasSession());
        const int previousSessions = sessions;
        coordinator.resetFault();
        QTRY_COMPARE(resets, 1); QVERIFY(sessions > previousSessions);
        QCOMPARE(tracking.preparedPlan(), plan); QVERIFY(tracking.busy());
        resetFinished = true;
        QTRY_VERIFY(!tracking.busy());
        if (applyReset) {
            QVERIFY(tracking.preparedPlan().isEmpty());
            QVERIFY(!tracking.control("resume")); // Old motion cannot survive a successful reset.
        } else QCOMPARE(tracking.preparedPlan(), plan);
        QTest::qWait(150); QCOMPARE(resets, 1); QCOMPARE(starts, 0);
        coordinator.shutdown();
    }
    void transientMonitorLossKeepsTheExplicitDeploymentIntent() {
        DeploymentFixture server;
        const auto normal = server.handler;
        bool failMonitor = false;
        int failures = 0;
        server.handler = [&](auto *socket, const QByteArray &path, const QJsonObject &body) {
            if (failMonitor && path.startsWith("/api/v1/status")) {
                ++failures; HttpFixture::reply(socket, {}, 503);
            } else normal(socket, path, body);
        };
        TrackingClient tracking; PoseClient pose; ControlSessionCoordinator coordinator(&tracking, &pose);
        MapFrameBinding binding; binding.confirmed = true;
        binding.context = {"map", "origin", "transform", "cal", "profile", 1};
        coordinator.setBinding(binding); tracking.configure(server.url(), {}, 250); tracking.start();
        QTRY_VERIFY(coordinator.coordinatesReady());
        QVERIFY(coordinator.uploadAndStart({{"taskId", "task"}, {"revision", 1},
                                           {"context", binding.context.toJson()}}));
        QTRY_COMPARE(server.uploads, 1);
        failMonitor = true;
        QTRY_VERIFY(failures > 0);
        QTest::qWait(40); // Deliver the failed response, not just its server-side acceptance.
        QVERIFY(tracking.commandFresh());
        QVERIFY(coordinator.actionPending());
        QVERIFY(!tracking.motionReady());
        failMonitor = false; server.prepareApplied = true; server.showReady();
        QTRY_COMPARE(server.starts, 1);
        QCOMPARE(server.uploads, 1);
        coordinator.shutdown();
    }
    void heartbeatLossKeepsOnlyTheKnownLeaseAndRecoversWithoutReacquisition() {
        DeploymentFixture server;
        const auto normal = server.handler;
        bool failHeartbeat = false;
        int failures = 0, acquisitions = 0;
        server.handler = [&](auto *socket, const QByteArray &path, const QJsonObject &body) {
            if (path.endsWith("/sessions")) ++acquisitions;
            if (failHeartbeat && path.endsWith("/heartbeat")) {
                ++failures; HttpFixture::reply(socket, {}, 503);
            } else normal(socket, path, body);
        };
        TrackingClient tracking; tracking.configure(server.url(), {}, 250); tracking.start();
        QTRY_VERIFY(tracking.fresh()); tracking.acquireSession();
        QTRY_VERIFY(tracking.motionReady());
        failHeartbeat = true;
        QTRY_VERIFY(failures > 0);
        QTRY_VERIFY(!tracking.motionReady());
        QVERIFY(tracking.hasSession());
        failHeartbeat = false;
        QTRY_VERIFY(tracking.motionReady());
        QCOMPARE(acquisitions, 1);
        failHeartbeat = true;
        QTRY_VERIFY_WITH_TIMEOUT(!tracking.hasSession(), 2500);
        QCOMPARE(acquisitions, 1); // Failed renewals never extend the locally known lease.
        tracking.stop();
    }
    void invalidStatusImmediatelyInvalidatesTheSnapshotAndSession_data() {
        QTest::addColumn<int>("code");
        QTest::newRow("unauthenticated") << 401;
        QTest::newRow("forbidden") << 403;
        QTest::newRow("invalid-status-document") << 200;
    }
    void invalidStatusImmediatelyInvalidatesTheSnapshotAndSession() {
        QFETCH(int, code);
        DeploymentFixture server; const auto normal = server.handler;
        bool invalid = false;
        server.handler = [&](auto *socket, const QByteArray &path, const QJsonObject &body) {
            if (invalid && path.startsWith("/api/v1/status")) HttpFixture::reply(socket, {}, code);
            else normal(socket, path, body);
        };
        TrackingClient tracking; tracking.configure(server.url(), {}, 250); tracking.start();
        QTRY_VERIFY(tracking.fresh()); tracking.acquireSession(); QTRY_VERIFY(tracking.hasSession());
        invalid = true;
        QTRY_VERIFY(!tracking.commandFresh());
        QVERIFY(!tracking.hasSession());
        QVERIFY(!tracking.motionReady());
        tracking.stop();
    }
    void heartbeatIdentityAndAuthenticationErrorsInvalidateTheSession_data() {
        QTest::addColumn<QString>("failure");
        QTest::newRow("forbidden") << QString("forbidden");
        QTest::newRow("wrong-boot") << QString("wrong-boot");
        QTest::newRow("invalid-document") << QString("invalid-document");
    }
    void heartbeatIdentityAndAuthenticationErrorsInvalidateTheSession() {
        QFETCH(QString, failure);
        DeploymentFixture server; const auto normal = server.handler;
        bool invalid = false;
        server.handler = [&](auto *socket, const QByteArray &path, const QJsonObject &body) {
            if (invalid && path.endsWith("/heartbeat")) {
                HttpFixture::reply(socket, failure == "wrong-boot" ? QJsonObject{
                    {"apiVersion", 1}, {"bootId", "other-boot"}, {"sessionId", "session"},
                    {"stateVersion", "1"}, {"motionPermit", "wrong"}, {"permitValidForMs", 3000}}
                    : QJsonObject{}, failure == "forbidden" ? 403 : 200);
            } else normal(socket, path, body);
        };
        TrackingClient tracking; tracking.configure(server.url(), {}, 250); tracking.start();
        QTRY_VERIFY(tracking.fresh()); tracking.acquireSession(); QTRY_VERIFY(tracking.motionReady());
        invalid = true;
        QTRY_VERIFY(!tracking.hasSession());
        QVERIFY(!tracking.motionReady());
        tracking.stop();
    }
    void delayedConfigurationFromThePreviousBootCannotRepopulateMetadata() {
        DeploymentFixture server; const auto normal = server.handler;
        QPointer<QTcpSocket> oldConfiguration;
        int configurationRequests = 0;
        server.handler = [&](auto *socket, const QByteArray &path, const QJsonObject &body) {
            if (path.endsWith("/configuration")) {
                if (++configurationRequests == 1) oldConfiguration = socket;
                else HttpFixture::reply(socket, {{"apiVersion", 1}, {"bootId", server.boot}, {"marker", "new"}});
            } else normal(socket, path, body);
        };
        TrackingClient tracking; tracking.configure(server.url(), {}, 3000); tracking.start();
        QSignalSpy changes(&tracking, &TrackingClient::configurationChanged);
        QTRY_VERIFY(oldConfiguration);
        server.boot = "new-boot";
        QTRY_COMPARE(tracking.snapshot().bootId, server.boot);
        // Old compatible services omitted bootId: the request generation must
        // still prevent this response from binding a new empty map to old metadata.
        HttpFixture::reply(oldConfiguration, {{"apiVersion", 1}, {"marker", "old"}});
        QTRY_COMPARE(tracking.configuration()["marker"].toString(), QString("new"));
        for (const auto &change : changes)
            QVERIFY(change.first().toJsonObject()["marker"].toString() != "old");
        tracking.stop();
    }
    void configurationWithTheWrongBootIsNotPublished() {
        DeploymentFixture server; const auto normal = server.handler;
        bool wrongBoot = true;
        int configurationRequests = 0;
        server.handler = [&](auto *socket, const QByteArray &path, const QJsonObject &body) {
            if (path.endsWith("/configuration")) {
                ++configurationRequests;
                HttpFixture::reply(socket, {{"apiVersion", 1}, {"bootId", wrongBoot ? "other-boot" : server.boot},
                                           {"marker", wrongBoot ? "wrong" : "current"}});
            } else normal(socket, path, body);
        };
        TrackingClient tracking; tracking.configure(server.url(), {}, 250); tracking.start();
        QTRY_VERIFY(configurationRequests > 0);
        QTest::qWait(50);
        QVERIFY(tracking.configuration().isEmpty());
        wrongBoot = false;
        QTRY_COMPARE(tracking.configuration()["marker"].toString(), QString("current"));
        tracking.stop();
    }
    void unchangedConnectionsPreserveSessionPoseAndInFlightPhoto() {
        HttpFixture server; QTemporaryDir journal;
        quint64 seq = 0, poseSeq = 0;
        int streams = 0, acquisitions = 0, photos = 0, acks = 0;
        bool waiting = false;
        QPointer<QTcpSocket> photoResponse;
        QHash<QString, QString> requestIds;
        const QJsonObject event{{"eventId", "execution/photo"}, {"executionId", "execution"},
            {"stepId", "photo"}, {"type", "external_action_requested"}, {"reason", "capture"}, {"seq", "1"}};
        server.handler = [&](auto *socket, const QByteArray &path, const QJsonObject &body) {
            if (path == "/api/v1/pose/stream") {
                ++streams; HttpFixture::stream(socket, [&] { return poseObject(QString::number(++poseSeq)); });
            } else if (path.startsWith("/api/v1/status")) {
                auto status = statusObject(++seq, "boot");
                if (waiting) {
                    status["state"] = "Waiting"; status["executionId"] = "execution";
                    status["stepId"] = "photo"; status["waitingEventId"] = "execution/photo";
                    status["lastEventSeq"] = "1";
                }
                HttpFixture::reply(socket, status);
            } else if (path.endsWith("/sessions")) {
                ++acquisitions;
                HttpFixture::reply(socket, {{"apiVersion", 1}, {"bootId", "boot"}, {"sessionId", "session"},
                    {"sessionToken", "token"}, {"expiresInMs", 3000}});
            } else if (path.endsWith("/heartbeat"))
                HttpFixture::reply(socket, {{"apiVersion", 1}, {"bootId", "boot"}, {"sessionId", "session"},
                    {"stateVersion", "1"}, {"motionPermit", "permit"}, {"permitValidForMs", 500}});
            else if (path.endsWith("/tasks") || path.endsWith("/ack")) {
                const auto id = body["requestId"].toString(); requestIds.insert(id, id);
                if (path.endsWith("/ack")) ++acks;
                HttpFixture::reply(socket, {{"commandId", id}}, 202);
            } else if (path.startsWith("/api/v1/commands")) {
                const auto id = path.startsWith("/api/v1/commands?") ?
                    QUrlQuery(QUrl(QString::fromUtf8(path))).queryItemValue("requestId") :
                    QString::fromUtf8(path.mid(QByteArray("/api/v1/commands/").size()));
                if (!requestIds.contains(id)) HttpFixture::reply(socket, {}, 404);
                else HttpFixture::reply(socket, {{"commandId", id}, {"requestId", id}, {"state", "applied"}});
            } else if (path.endsWith("/camera/photo")) {
                ++photos; photoResponse = socket;
            } else if (path.startsWith("/api/v1/events"))
                HttpFixture::reply(socket, {{"apiVersion", 1}, {"bootId", "boot"},
                    {"events", waiting ? QJsonArray{event} : QJsonArray{}}});
            else HttpFixture::reply(socket, {{"apiVersion", 1}, {"bootId", "boot"}, {"ready", true}});
        };
        TrackingClient tracking; PoseClient pose; ExternalEventCoordinator external(&tracking);
        tracking.configure(server.url(), {}, 3000); tracking.start();
        pose.configure(server.url(), {}, 3000); pose.start();
        external.configure(server.url(), {}, journal.path(), 3000);
        QTRY_VERIFY(tracking.fresh() && pose.fresh());
        tracking.acquireSession(); QTRY_VERIFY(tracking.hasSession());
        QTRY_VERIFY(tracking.configurationFresh());
        const QJsonObject plan{{"taskId", "task"}, {"revision", 1}};
        QVERIFY(tracking.upload(plan)); QTRY_VERIFY(!tracking.busy());
        waiting = true; QTRY_VERIFY(photoResponse);
        QVERIFY(external.manualConfirmationEventId().isEmpty());
        QVERIFY(!external.resolveCurrent("execution/photo", true));
        tracking.configure(server.url(), {}, 3000); tracking.start();
        pose.configure(server.url(), {}, 3000); pose.start();
        external.configure(server.url(), {}, journal.path(), 3000);
        QVERIFY(tracking.hasSession()); QCOMPARE(tracking.preparedPlan(), plan);
        QVERIFY(pose.fresh()); QCOMPARE(streams, 1);
        HttpFixture::reply(photoResponse, {{"success", true}, {"path", "Capture/photo.jpg"}});
        QTRY_COMPARE(acks, 1);
        QVERIFY(external.manualConfirmationEventId().isEmpty());
        QCOMPARE(photos, 1); QCOMPARE(acquisitions, 1); QCOMPARE(streams, 1);
        tracking.stop(); pose.stop();
    }
    void stopBypassesExpiredMonitoringSessionAndSaturatedOrdinaryConnections() {
        HttpFixture server;
        quint64 seq = 0;
        bool blocked = false, complete = false;
        int acquisitions = 0, safetyQueries = 0;
        QJsonArray stops;
        QHash<QString, QString> operations;
        auto liveStatus = [&] {
            auto status = statusObject(++seq, "boot");
            status["state"] = "Tracking";
            status["executionId"] = "execution";
            status["lastEventSeq"] = "1";
            return status;
        };
        server.handler = [&](auto *socket, const QByteArray &path, const QJsonObject &body) {
            if (path.endsWith("/control")) {
                QCOMPARE(HttpFixture::requestHeader(socket, "Authorization"), QByteArray("Bearer secret"));
                QVERIFY(HttpFixture::requestHeader(socket, "X-Operator-Token").isEmpty());
                const auto op = body["cmd"].toString();
                operations[body["requestId"].toString()] = op;
                stops.append(body);
                if (op == "pause") socket->abort(); // Acceptance response lost: query, never replay.
                else HttpFixture::reply(socket, {{"commandId", op}}, 202);
                return;
            }
            if (path.startsWith("/api/v1/commands")) {
                const QUrlQuery query(QUrl(QString::fromUtf8(path)));
                const auto session = query.queryItemValue("sessionId");
                const auto op = path.startsWith("/api/v1/commands?") ?
                    operations.value(query.queryItemValue("requestId")) :
                    QString::fromUtf8(path.mid(QByteArray("/api/v1/commands/").size()));
                if (QStringList{"pause", "abort", "estop"}.contains(op)) {
                    if (op == "pause" && path.startsWith("/api/v1/commands?"))
                        QCOMPARE(session, QString("safety"));
                    ++safetyQueries;
                    HttpFixture::reply(socket, {{"commandId", op}, {"state", complete ? "applied" : "accepted"}});
                    return;
                }
                if (session == "safety" || session == "emergency") {
                    // Discovery can beat POST acceptance on a separate connection.
                    HttpFixture::reply(socket, {}, 404);
                    return;
                }
            }
            // Occupy status, metadata, event, write and ordinary result lanes.
            if (blocked) return;
            if (path.startsWith("/api/v1/status")) HttpFixture::reply(socket, liveStatus());
            else if (path.endsWith("/sessions")) {
                ++acquisitions;
                HttpFixture::reply(socket, {{"apiVersion", 1}, {"bootId", "boot"}, {"sessionId", "session"},
                    {"sessionToken", "token"}, {"expiresInMs", 1000}});
            } else if (path.endsWith("/heartbeat"))
                HttpFixture::reply(socket, {{"apiVersion", 1}, {"bootId", "boot"}, {"sessionId", "session"},
                    {"stateVersion", "1"}, {"motionPermit", "permit"}, {"permitValidForMs", 500}});
            else HttpFixture::reply(socket, {{"apiVersion", 1}, {"bootId", "boot"}, {"ready", true}, {"events", QJsonArray{}}});
        };
        TrackingClient client;
        client.configure(server.url(), "secret", 5000); client.start();
        QTRY_VERIFY(client.fresh()); client.acquireSession(); QTRY_VERIFY(client.hasSession());
        QTRY_VERIFY(client.configurationFresh());
        client.stopRenewingSession();
        blocked = true;
        QVERIFY(client.upload({{"taskId", "task"}, {"revision", 1}}));
        QTest::qWait(3250);
        QVERIFY(!client.commandFresh()); QVERIFY(!client.hasSession()); QVERIFY(client.busy());
        QVERIFY(!client.control("start"));
        QVERIFY(client.control("pause"));
        QVERIFY(!client.control("pause")); // Same unresolved stop is bounded, not queued.
        QVERIFY(client.control("abort"));
        QVERIFY(client.control("estop")); // Pending pause/abort and ordinary work cannot block it.
        QTRY_COMPARE_WITH_TIMEOUT(stops.size(), 3, 1000);
        QCOMPARE(acquisitions, 1);
        for (const auto &entry : stops) {
            const auto body = entry.toObject();
            if (body["cmd"] != "estop") {
                QVERIFY(body["safetyStop"].toBool());
                QCOMPARE(body["sessionId"].toString(), QString("safety"));
                QCOMPARE(body["bootId"].toString(), QString("boot"));
                QCOMPARE(body["taskId"].toString(), QString("task"));
                QCOMPARE(body["revision"].toInt(), 1);
                QCOMPARE(body["executionId"].toString(), QString("execution"));
                QVERIFY(!body.contains("motionPermit"));
            }
        }
        QTRY_VERIFY_WITH_TIMEOUT(safetyQueries >= 3, 1000);
        QSignalSpy finished(&client, &TrackingClient::commandFinished);
        complete = true;
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 3, 1000);
        client.stop();
        blocked = false; client.start();
        QTRY_VERIFY(client.fresh()); QTest::qWait(350);
        QCOMPARE(stops.size(), 3); // Neither reconnect nor lost acceptance can replay stop or motion.
        QVERIFY(!client.hasSession());
        client.stop();
    }
    void stopMessagesRequireFreshMeasuredStopRatherThanAcceptanceOrZeroNumbers() {
        HttpFixture server;
        TrackingClient tracking; PoseClient pose; ControlSessionCoordinator coordinator(&tracking, &pose);
        QSignalSpy messages(&coordinator, &ControlSessionCoordinator::message);
        coordinator.emergencyStop();
        QVERIFY(messages.last().first().toString().contains("未发送"));
        quint64 seq = 0;
        bool accepted = false, applied = false, valid = false, stopped = false, connected = true;
        QString state = "Tracking", request;
        QPointer<QTcpSocket> acceptance;
        server.handler = [&](auto *socket, const QByteArray &path, const QJsonObject &body) {
            if (path.startsWith("/api/v1/status")) {
                auto status = statusObject(++seq, "boot");
                status["state"] = state; status["executionId"] = "execution";
                status["chassisConnected"] = connected; status["measuredStopped"] = stopped;
                status["measuredTwist"] = QJsonObject{{"v", 0}, {"omega", 0}, {"valid", valid}};
                HttpFixture::reply(socket, status);
            } else if (path.endsWith("/control")) {
                request = body["requestId"].toString();
                acceptance = socket;
            } else if (path.startsWith("/api/v1/commands"))
                HttpFixture::reply(socket, accepted ? QJsonObject{{"commandId", "stop"}, {"requestId", request},
                    {"state", applied ? "applied" : "accepted"}} : QJsonObject{});
            else HttpFixture::reply(socket, {{"apiVersion", 1}, {"bootId", "boot"}, {"events", QJsonArray{}}});
        };
        tracking.configure(server.url(), {}, 500); tracking.start(); QTRY_VERIFY(tracking.fresh());
        messages.clear(); coordinator.emergencyStop();
        QVERIFY(messages.last().first().toString().contains("等待工控机确认"));
        auto contains = [&](const QString &text) {
            for (const auto &message : messages) if (message.first().toString().contains(text)) return true;
            return false;
        };
        QTRY_VERIFY(acceptance); QTest::qWait(150);
        QVERIFY(!contains("已受理")); // HTTP 200 alone with an empty result is not acceptance proof.
        accepted = true;
        HttpFixture::reply(acceptance, {{"commandId", "stop"}}, 202);
        QTRY_VERIFY(contains("已受理"));
        QVERIFY(!contains("实测已停稳"));
        applied = true; QTRY_VERIFY(contains("已应用"));
        state = "Fault"; QTRY_COMPARE(tracking.snapshot().state, QString("Fault"));
        QTest::qWait(150); QVERIFY(!contains("实测已停稳"));
        valid = true; stopped = true; connected = false;
        QTRY_VERIFY(tracking.snapshot().measuredStopped && !tracking.snapshot().chassisConnected);
        QVERIFY(!contains("实测已停稳"));
        connected = true; QTRY_VERIFY(contains("实测已停稳"));
        coordinator.shutdown();
    }
    void measuredStopStatusIsOptionalAndValidated() {
        TrackingSnapshot status;
        auto object = statusObject(1, "boot");
        QVERIFY(TrackingJson::status(object, &status));
        QVERIFY(!status.measuredStopped); QVERIFY(!status.measuredValid);
        object["measuredStopped"] = true; object["chassisConnected"] = true;
        object["measuredTwist"] = QJsonObject{{"v", 0}, {"omega", 0}, {"valid", true}};
        QVERIFY(TrackingJson::status(object, &status));
        QVERIFY(status.measuredStopped); QVERIFY(status.measuredValid); QVERIFY(status.chassisConnected);
        object["measuredStopped"] = "true";
        QVERIFY(!TrackingJson::status(object, &status));
        object["measuredStopped"] = true;
        object["measuredTwist"] = QJsonObject{{"v", 0}, {"omega", 0}, {"valid", 1}};
        QVERIFY(!TrackingJson::status(object, &status));
    }
    void wirelessGapKeepsMonitoringButExpiredStatusCannotStartOrReplay() {
        DeploymentFixture server;
        const auto original = server.handler;
        bool hold = false;
        int controls = 0;
        QPointer<QTcpSocket> delayed;
        server.handler = [&](auto *socket, const QByteArray &path, const QJsonObject &body) {
            if (path.endsWith("/control")) ++controls;
            if (hold && path.startsWith("/api/v1/status")) delayed = socket;
            else original(socket, path, body);
        };
        TrackingClient tracking; PoseClient pose; ControlSessionCoordinator coordinator(&tracking, &pose);
        MapFrameBinding binding; binding.confirmed = true;
        binding.context = {"map", "origin", "transform", "cal", "profile", 1};
        coordinator.setBinding(binding);
        tracking.configure(server.url(), {}, 5000); tracking.start();
        QTRY_VERIFY(coordinator.coordinatesReady());
        hold = true;
        QTRY_VERIFY(delayed);
        QTest::qWait(2450);
        QVERIFY(!tracking.fresh());
        QVERIFY(tracking.commandFresh());
        QVERIFY(tracking.connected());
        QSignalSpy availability(&tracking, &TrackingClient::availabilityChanged);
        QTest::qWait(800);
        QVERIFY(!tracking.commandFresh());
        QVERIFY(tracking.connected());
        QVERIFY(!coordinator.uploadAndStart({{"taskId", "task"}, {"revision", 1},
                                             {"context", binding.context.toJson()}}));
        QVERIFY(!tracking.control("start"));
        QTRY_VERIFY(!availability.isEmpty());
        QVERIFY(availability.last()[1].toString().contains("延迟"));
        hold = false;
        auto status = statusObject(++server.seq, server.boot);
        status["state"] = server.state; status["taskId"] = server.task;
        HttpFixture::reply(delayed, status);
        QTRY_VERIFY(tracking.fresh());
        QTest::qWait(300);
        QCOMPARE(server.uploads, 0); QCOMPARE(server.starts, 0); QCOMPARE(controls, 0);
        coordinator.shutdown();
    }
    void wirelessPoseGapKeepsStreamButNeverMakesOldPoseFresh() {
        HttpFixture server;
        quint64 seq = 0;
        int requests = 0;
        bool publish = true;
        server.handler = [&](auto *socket, const QByteArray &, const QJsonObject &) {
            ++requests;
            HttpFixture::stream(socket, [&] {
                return publish ? poseObject(QString::number(++seq)) : QJsonObject{};
            });
        };
        PoseClient client; client.configure(server.url(), {});
        QSignalSpy errors(&client, &PoseClient::errorOccurred);
        client.start(); QTRY_VERIFY(client.fresh());
        publish = false;
        QTest::qWait(2500);
        QVERIFY(!client.fresh());
        QVERIFY(!client.snapshot().validForControl);
        // The last already queued TCP sample may be delivered just after the
        // source is paused. It must still be visibly old, never zero-aged.
        QVERIFY(client.snapshot().stateAgeMs > 2000);
        QCOMPARE(requests, 1); QCOMPARE(errors.size(), 0);
        publish = true;
        QTRY_VERIFY(client.fresh());
        QCOMPARE(requests, 1);
        client.stop();
    }
    void delayedSessionAndCommandResultUseServerLeaseWithoutResending() {
        DeploymentFixture server;
        const auto original = server.handler;
        bool delayedHeartbeat = false;
        bool delayedHeartbeatReplied = false;
        QPointer<QTcpSocket> heartbeat;
        QJsonObject startBody;
        server.handler = [&](auto *socket, const QByteArray &path, const QJsonObject &body) {
            if (path.endsWith("/sessions")) {
                QTimer::singleShot(1200, socket, [&, socket] {
                    HttpFixture::reply(socket, {{"apiVersion", 1}, {"bootId", server.boot},
                        {"sessionId", "session"}, {"sessionToken", "token"}, {"expiresInMs", 10000}});
                });
            } else if (path.endsWith("/heartbeat")) {
                auto reply = [&, socket] {
                    HttpFixture::reply(socket, {{"apiVersion", 1}, {"bootId", server.boot},
                        {"sessionId", "session"}, {"stateVersion", QString::number(server.version)},
                        {"expiresInMs", 10000}, {"motionPermit", "permit"}, {"permitValidForMs", 3000}});
                };
                if (delayedHeartbeat) {
                    heartbeat = socket;
                    QTimer::singleShot(2500, socket, [&, reply] {
                        reply();
                        delayedHeartbeatReplied = true;
                    });
                }
                else reply();
            } else if (path.endsWith("/control") && body["cmd"] == "start") {
                startBody = body;
                // Accepted immediately; both POST and result are observed late.
                ++server.starts; server.startRequest = body["requestId"].toString();
                server.state = "Tracking"; server.execution = "execution"; ++server.version;
                QTimer::singleShot(2500, socket, [socket] { HttpFixture::reply(socket, {}, 503); });
            } else if (path.startsWith("/api/v1/commands") && !server.startRequest.isEmpty()) {
                ++server.startQueries;
                const auto id = server.startRequest;
                QTimer::singleShot(2500, socket, [socket, id] {
                    HttpFixture::reply(socket, {{"state", "applied"}, {"commandId", "start"}, {"requestId", id}});
                });
            } else original(socket, path, body);
        };
        TrackingClient client;
        client.configure(server.url(), {}); client.start();
        QTRY_VERIFY(client.fresh()); client.acquireSession();
        QTRY_VERIFY(client.hasSession()); QTRY_VERIFY(client.motionReady());
        // Install the known prepared task without changing any IPC motion.
        QVERIFY(client.upload({{"taskId", "task"}, {"revision", 1}}));
        QTRY_COMPARE(server.uploads, 1);
        server.prepareApplied = true; server.showReady();
        QTRY_VERIFY(!client.busy()); QTRY_VERIFY(client.motionReady());
        delayedHeartbeat = true;
        QTRY_VERIFY(heartbeat);
        QTest::qWait(1300);
        QVERIFY(client.hasSession()); // The former one-second client cap would expire.
        delayedHeartbeat = false;
        // A delayed permit can legitimately have less than 500 ms remaining.
        // Require a new state version after that reply so motionReady cannot
        // pass using it while the immediate renewal is still in flight.
        QTRY_VERIFY(delayedHeartbeatReplied);
        ++server.version;
        QTRY_COMPARE(client.snapshot().stateVersion, server.version);
        QTRY_VERIFY(client.motionReady());
        QVERIFY(client.control("start"));
        QTRY_COMPARE(server.starts, 1);
        QVERIFY(startBody["validForMs"].toInt() > 500);
        QVERIFY(startBody["validForMs"].toInt() <= 3000);
        QTRY_VERIFY_WITH_TIMEOUT(!client.busy(), 5000);
        QTest::qWait(400);
        QCOMPARE(server.starts, 1); QVERIFY(server.startQueries >= 1);
        client.stop();
    }
    void slowFailedSessionDoesNotConsumeUnsentRetryOrReplayMotion() {
        DeploymentFixture server;
        const auto original = server.handler;
        int sessions = 0;
        qint64 firstResponseDelayMs = 0;
        QElapsedTimer firstRequestAge;
        server.handler = [&](auto *socket, const QByteArray &path, const QJsonObject &body) {
            if (path.endsWith("/sessions") && ++sessions == 1) {
                firstRequestAge.start();
                QTimer::singleShot(2000, socket, [&, socket] {
                    firstResponseDelayMs = firstRequestAge.elapsed();
                    HttpFixture::reply(socket, {}, 503);
                });
            } else original(socket, path, body);
        };
        TrackingClient tracking; PoseClient pose;
        ControlSessionCoordinator coordinator(&tracking, &pose);
        MapFrameBinding binding; binding.confirmed = true;
        binding.context = {"map", "origin", "transform", "cal", "profile", 1};
        coordinator.setBinding(binding);
        tracking.configure(server.url(), {}, 5000); tracking.start();
        QTRY_VERIFY(coordinator.coordinatesReady());
        QVERIFY(coordinator.uploadAndStart({{"taskId", "task"}, {"revision", 1},
                                           {"context", binding.context.toJson()}}));
        QTRY_COMPARE(sessions, 1);
        QVERIFY(!tracking.acquireSession());
        QTest::qWait(1600);
        QCOMPARE(sessions, 1); // The first request still owns the session lane.
        QVERIFY(coordinator.actionPending());
        QTRY_COMPARE_WITH_TIMEOUT(server.uploads, 1, 5000);
        QVERIFY(firstResponseDelayMs >= 1500);
        QCOMPARE(sessions, 2); // Only requests actually issued consume either attempt.
        server.prepareApplied = true; server.showReady();
        QTRY_COMPARE(server.starts, 1);
        QTRY_VERIFY(!coordinator.actionPending());
        QTRY_VERIFY(!tracking.busy());
        server.offline = true;
        QTRY_VERIFY(!tracking.commandFresh());
        server.showReady(); // Reconnection to a startable task is not another click.
        server.offline = false;
        QTRY_VERIFY(tracking.commandFresh());
        QTest::qWait(350);
        QCOMPARE(server.uploads, 1);
        QCOMPARE(server.starts, 1);
        QVERIFY(!coordinator.actionPending());
        coordinator.shutdown();
    }
    void returnToStartStatusIsOptionalAndValidated() {
        TrackingSnapshot status;
        auto object = statusObject(1, "boot");
        QVERIFY(TrackingJson::status(object, &status));
        QVERIFY(!status.returningToStart);
        object["returningToStart"] = true;
        QVERIFY(TrackingJson::status(object, &status));
        QVERIFY(status.returningToStart);
        object["returningToStart"] = "true";
        QVERIFY(!TrackingJson::status(object, &status));
        QVERIFY(TrackingJson::reasonText("unsupported_step_type").contains(QStringLiteral("更新")));
        QVERIFY(TrackingJson::reasonText("normal_handover_unstable").contains(QStringLiteral("手动继续")));
    }
    void turnRecoveryStatusIsOptionalAndValidated() {
        TrackingSnapshot status;
        auto object = statusObject(1, "boot");
        QVERIFY(TrackingJson::status(object, &status));
        QVERIFY(!status.turnRecovering); QCOMPARE(status.turnOffsetMeters, 0.0);
        object["turnRecovering"] = true; object["turnOffsetMeters"] = .12;
        QVERIFY(TrackingJson::status(object, &status));
        QVERIFY(status.turnRecovering); QCOMPARE(status.turnOffsetMeters, .12);
        object["turnRecovering"] = "true";
        QVERIFY(!TrackingJson::status(object, &status));
        object["turnRecovering"] = true; object["turnOffsetMeters"] = -.1;
        QVERIFY(!TrackingJson::status(object, &status));
    }
    void oneClickWaitsForAppliedAndReadyAndDoesNotRepeatLostStart() {
        DeploymentFixture server;
        server.lostResponses = true;
        TrackingClient tracking; PoseClient pose;
        ControlSessionCoordinator coordinator(&tracking, &pose);
        MapFrameBinding binding; binding.confirmed = true;
        binding.context = {"map", "origin", "transform", "cal", "profile", 1};
        coordinator.setBinding(binding);
        tracking.configure(server.url(), {}, 200); tracking.start();
        QTRY_VERIFY(coordinator.coordinatesReady());
        const QJsonObject plan{{"taskId", "frozen-task"}, {"revision", 1},
                              {"context", binding.context.toJson()}};
        QVERIFY(coordinator.uploadAndStart(plan));
        QVERIFY(!coordinator.uploadAndStart(plan)); // Double clicking cannot create another intention.
        QTRY_COMPARE(server.uploads, 1);
        server.showReady();
        QTRY_COMPARE(tracking.snapshot().state, QString("Ready"));
        QTest::qWait(300);
        QCOMPARE(server.starts, 0); // Ready alone is insufficient; persistence must complete.
        QVERIFY(coordinator.actionPending());
        emit tracking.commandFinished("prepare", {{"requestId", "unrelated"}, {"state", "applied"}});
        QTest::qWait(100);
        QCOMPARE(server.starts, 0);
        server.prepareApplied = true;
        QTRY_COMPARE(server.starts, 1);
        QTRY_VERIFY(!coordinator.actionPending());
        QTRY_VERIFY(server.startQueries > 0);
        QTRY_VERIFY(!tracking.busy());
        QTest::qWait(350);
        QCOMPARE(server.uploads, 1);
        QCOMPARE(server.starts, 1); // Missing acceptance responses cause GET queries, never another POST.
        QCOMPARE(server.plan["context"], plan["context"]);
        coordinator.shutdown();
    }
    void oneClickNeedsReadyAfterPrepareCompletion() {
        DeploymentFixture server; server.prepareApplied = true;
        server.omitRequestId = true; // Older command responses inherit their locally queried identity.
        TrackingClient tracking; PoseClient pose; ControlSessionCoordinator coordinator(&tracking, &pose);
        MapFrameBinding binding; binding.confirmed = true;
        binding.context = {"map", "origin", "transform", "cal", "profile", 1};
        coordinator.setBinding(binding); tracking.configure(server.url(), {}, 200); tracking.start();
        QTRY_VERIFY(coordinator.coordinatesReady());
        QVERIFY(coordinator.uploadAndStart({{"taskId", "task"}, {"revision", 1},
                                           {"context", binding.context.toJson()}}));
        QTRY_COMPARE(server.uploads, 1); QTRY_VERIFY(!tracking.busy());
        QTest::qWait(250); QCOMPARE(server.starts, 0);
        server.showReady();
        QTRY_COMPARE(server.starts, 1);
        coordinator.shutdown();
    }
    void oneClickRejectsAnotherRequestsResult() {
        DeploymentFixture server; server.prepareApplied = true; server.mismatchResult = true;
        TrackingClient tracking; PoseClient pose; ControlSessionCoordinator coordinator(&tracking, &pose);
        MapFrameBinding binding; binding.confirmed = true;
        binding.context = {"map", "origin", "transform", "cal", "profile", 1};
        coordinator.setBinding(binding); tracking.configure(server.url(), {}, 200); tracking.start();
        QTRY_VERIFY(coordinator.coordinatesReady());
        QVERIFY(coordinator.uploadAndStart({{"taskId", "task"}, {"revision", 1},
                                           {"context", binding.context.toJson()}}));
        QTRY_COMPARE(server.uploads, 1); server.showReady();
        QTest::qWait(500); QCOMPARE(server.starts, 0); QVERIFY(tracking.busy());
        server.mismatchResult = false;
        QTRY_COMPARE(server.starts, 1);
        coordinator.shutdown();
    }
    void oneClickCancelledOnRejectedOrUnknownPrepare_data() {
        QTest::addColumn<bool>("reject");
        QTest::newRow("rejected") << true;
        QTest::newRow("unknown-after-timeout") << false;
    }
    void oneClickCancelledOnRejectedOrUnknownPrepare() {
        QFETCH(bool, reject);
        DeploymentFixture server; server.rejectPrepare = reject;
        TrackingClient tracking; PoseClient pose; ControlSessionCoordinator coordinator(&tracking, &pose);
        MapFrameBinding binding; binding.confirmed = true;
        binding.context = {"map", "origin", "transform", "cal", "profile", 1};
        coordinator.setBinding(binding); tracking.configure(server.url(), {}, 200); tracking.start();
        QTRY_VERIFY(coordinator.coordinatesReady());
        QVERIFY(coordinator.uploadAndStart({{"taskId", "task"}, {"revision", 1},
                                           {"context", binding.context.toJson()}}));
        QTRY_COMPARE(server.uploads, 1); server.showReady();
        QTRY_VERIFY_WITH_TIMEOUT(!coordinator.actionPending(), 12000);
        server.prepareApplied = true; server.rejectPrepare = false;
        QTest::qWait(300);
        QCOMPARE(server.starts, 0); QCOMPARE(server.uploads, 1);
        coordinator.shutdown();
    }
    void oneClickCancelsWithoutReplaying_data() {
        QTest::addColumn<QString>("interruption");
        for (const auto *value : {"disconnect", "restart", "replacement", "manual_cancel", "pause", "estop", "binding"})
            QTest::newRow(value) << QString(value);
    }
    void oneClickCancelsWithoutReplaying() {
        QFETCH(QString, interruption);
        DeploymentFixture server;
        TrackingClient tracking; PoseClient pose; ControlSessionCoordinator coordinator(&tracking, &pose);
        MapFrameBinding binding; binding.confirmed = true;
        binding.context = {"map", "origin", "transform", "cal", "profile", 1};
        coordinator.setBinding(binding); tracking.configure(server.url(), {}, 200); tracking.start();
        QTRY_VERIFY(coordinator.coordinatesReady());
        QVERIFY(coordinator.uploadAndStart({{"taskId", "task"}, {"revision", 1},
                                           {"context", binding.context.toJson()}}));
        QTRY_COMPARE(server.uploads, 1);
        if (interruption == "disconnect") { server.offline = true; QTRY_VERIFY(!tracking.fresh()); }
        else if (interruption == "restart") server.boot = "new-boot";
        else if (interruption == "replacement") { server.task = "someone-elses-task"; ++server.version; }
        else if (interruption == "manual_cancel") coordinator.cancelPendingAction();
        else if (interruption == "pause") coordinator.pauseTask();
        else if (interruption == "estop") coordinator.emergencyStop();
        else { binding.context.mapRevision = 2; coordinator.setBinding(binding); }
        QTRY_VERIFY(!coordinator.actionPending());
        server.offline = false; server.prepareApplied = true; server.showReady();
        QTRY_VERIFY(tracking.fresh());
        QTest::qWait(400);
        QCOMPARE(server.uploads, 1); QCOMPARE(server.starts, 0);
        coordinator.shutdown();
    }
    void explicitTaskIntentAcquiresSessionAndNeverReplaysAfterDisconnect() {
        HttpFixture server;
        quint64 seq = 0;
        const QString boot = "tracking-a";
        QString taskState = "Idle", taskId;
        int sessions = 0, starts = 0, uploads = 0;
        bool offline = false, delaySession = false;
        QPointer<QTcpSocket> sessionReply;
        QJsonObject savedPlan;
        auto replySession = [&](QTcpSocket *socket) {
            HttpFixture::reply(socket, {{"apiVersion", 1}, {"bootId", boot}, {"sessionId", "session"},
                                       {"sessionToken", "token"}, {"expiresInMs", 1000}});
        };
        server.handler = [&](auto *socket, const QByteArray &path, const QJsonObject &body) {
            if (path.startsWith("/api/v1/status")) {
                auto status = statusObject(++seq, boot);
                status["state"] = taskState;
                status["taskId"] = taskId;
                HttpFixture::reply(socket, status, offline ? 503 : 200);
            } else if (path.endsWith("/health"))
                HttpFixture::reply(socket, {{"apiVersion", 1}, {"bootId", boot}, {"ready", true}});
            else if (path.endsWith("/configuration"))
                HttpFixture::reply(socket, {{"apiVersion", 1}, {"profile_revision", "profile"},
                    {"calibration", QJsonObject{{"calibrationId", "cal"}}},
                    {"origin", QJsonObject{{"active", QJsonObject{{"originRevision", "origin"}}}}},
                    {"navigation_defaults", QJsonObject{{"rotation_policy", "vehicle_footprint"},
                                                       {"default_safety_profile", "field-low-speed"}}}});
            else if (path.endsWith("/sessions")) {
                ++sessions;
                if (delaySession) sessionReply = socket;
                else replySession(socket);
            } else if (path.endsWith("/heartbeat"))
                HttpFixture::reply(socket, {{"apiVersion", 1}, {"bootId", boot}, {"sessionId", "session"},
                    {"stateVersion", "1"}, {"motionPermit", "permit"}, {"permitValidForMs", 500}});
            else if (path.endsWith("/tasks")) {
                ++uploads;
                savedPlan = body;
                taskId = body["taskId"].toString();
                taskState = "Ready";
                HttpFixture::reply(socket, {{"commandId", "prepare"}}, 202);
            } else if (path.endsWith("/control")) {
                if (body["cmd"].toString() == "start") ++starts;
                HttpFixture::reply(socket, {{"commandId", "start"}}, 202);
            } else if (path.startsWith("/api/v1/commands"))
                HttpFixture::reply(socket, {{"state", "applied"}});
            else HttpFixture::reply(socket, {{"apiVersion", 1}, {"bootId", boot}, {"events", QJsonArray{}}});
        };
        TrackingClient tracking;
        PoseClient pose; // Wireless pose preview is deliberately absent; IPC owns local readiness.
        ControlSessionCoordinator coordinator(&tracking, &pose);
        MapFrameBinding binding;
        binding.confirmed = true;
        binding.context = {"map", "origin", "transform", "cal", "profile", 1};
        coordinator.setBinding(binding);
        // Keep the deliberately held session response alive past the three-second
        // wireless command window; cancellation must be caused by stale status.
        tracking.configure(server.url(), {}, 10000);
        tracking.start();
        QTRY_VERIFY(coordinator.coordinatesReady());
        QVERIFY(!pose.fresh());
        const QJsonObject plan{{"taskId", "task"}, {"revision", 1}, {"context", binding.context.toJson()}};
        QVERIFY(coordinator.upload(plan));
        QTRY_COMPARE(uploads, 1);
        QTRY_VERIFY(!tracking.busy());
        QTRY_COMPARE(tracking.snapshot().state, QString("Ready"));
        QCOMPARE(savedPlan["context"], plan["context"]);
        QCOMPARE(sessions, 1);
        QCOMPARE(starts, 0); // Upload never starts motion.
        QVERIFY(tracking.releaseSessionForMaintenance());
        delaySession = true;
        coordinator.startTask();
        QTRY_VERIFY(sessionReply);
        QVERIFY(coordinator.actionPending());
        offline = true;
        QTRY_VERIFY(!tracking.fresh());
        QTRY_VERIFY(!coordinator.actionPending());
        offline = false;
        QTRY_VERIFY(tracking.fresh());
        QVERIFY(sessionReply);
        replySession(sessionReply);
        sessionReply.clear();
        QTest::qWait(350);
        QCOMPARE(starts, 0); // Late acquisition and reconnection cannot replay a start.
        delaySession = false;
        coordinator.startTask();
        QTRY_COMPARE(starts, 1);
        QTRY_VERIFY(!tracking.busy());
        QTest::qWait(350);
        QCOMPARE(starts, 1);
        coordinator.shutdown();
    }
    void recoveryStatusIsPreservedAndValidated() {
        TrackingSnapshot snapshot;
        auto object = statusObject(1, "boot");
        QVERIFY(TrackingJson::status(object, &snapshot));
        QVERIFY(!snapshot.autoResumePending);
        QCOMPARE(snapshot.recoveryStableMs, 0.0);
        object["state"] = "Paused";
        object["autoResumePending"] = true;
        object["recoveryStableMs"] = 250;
        QVERIFY(TrackingJson::status(object, &snapshot));
        QVERIFY(snapshot.autoResumePending);
        QCOMPARE(snapshot.recoveryStableMs, 250.0);
        object["autoResumePending"] = "true";
        QVERIFY(!TrackingJson::status(object, &snapshot));
        object["autoResumePending"] = true;
        object["recoveryStableMs"] = -1;
        QVERIFY(!TrackingJson::status(object, &snapshot));
    }
    void eventPollingFollowsAdvertisedSequenceAndRecovers() {
        HttpFixture server;
        quint64 seq = 0, advertised = 0;
        QString boot = "boot-a";
        bool offline = false, gap = false, legacy = false;
        int eventRequests = 0, metadataRequests = 0;
        server.handler = [&](auto *socket, const QByteArray &path, const QJsonObject &) {
            if (path.startsWith("/api/v1/status")) {
                auto status = statusObject(++seq, boot);
                status["lastEventSeq"] = QString::number(advertised);
                if (legacy) status.remove("lastEventSeq");
                HttpFixture::reply(socket, status, offline ? 503 : 200);
            } else if (path.startsWith("/api/v1/events")) {
                ++eventRequests;
                if (legacy) {
                    HttpFixture::reply(socket, {{"apiVersion", 1}, {"bootId", boot}, {"events", QJsonArray{}}});
                    return;
                }
                if (gap) {
                    gap = false;
                    QVERIFY(path.contains("after=0"));
                    HttpFixture::reply(socket, {{"code", "event_cursor_expired"}}, 410);
                    return;
                }
                QJsonArray events;
                if (boot == "boot-a") {
                    // Paginated burst: status advertises 3, first page only has 1.
                    if (path.contains("after=0")) events.append(QJsonObject{{"seq", "1"}});
                    else if (path.contains("after=1")) {
                        events.append(QJsonObject{{"seq", "2"}});
                        events.append(QJsonObject{{"seq", "3"}});
                    } else QFAIL("queried an already consumed cursor");
                } else {
                    QVERIFY(path.contains("after=2"));
                    events.append(QJsonObject{{"seq", "3"}});
                }
                HttpFixture::reply(socket, {{"apiVersion", 1}, {"bootId", boot}, {"events", events}});
            } else if (path.endsWith("/health") || path.endsWith("/configuration")) {
                ++metadataRequests;
                HttpFixture::reply(socket, {{"apiVersion", 1}, {"bootId", boot}, {"ready", true}});
            } else HttpFixture::reply(socket, {}, 404);
        };
        TrackingClient client;
        QSignalSpy events(&client, &TrackingClient::eventReceived);
        QSignalSpy resync(&client, &TrackingClient::eventsResynchronized);
        client.configure(server.url(), {}, 200);
        client.start();
        QTRY_VERIFY(client.fresh());
        QTRY_VERIFY(metadataRequests >= 2);
        QTest::qWait(600);
        QCOMPARE(eventRequests, 0);
        advertised = 3;
        QTRY_COMPARE(events.count(), 3);
        QCOMPARE(eventRequests, 2);
        QTest::qWait(450);
        QCOMPARE(eventRequests, 2);
        offline = true;
        QTRY_VERIFY(!client.fresh());
        QTest::qWait(250); // Allow requests accepted while fresh to finish.
        const int offlineMetadata = metadataRequests;
        QTest::qWait(1100);
        QCOMPARE(metadataRequests, offlineMetadata);
        // New boot resets the old cursor; an expired retained page resynchronizes.
        boot = "boot-b"; advertised = 2; gap = true; offline = false;
        QTRY_COMPARE(resync.count(), 1);
        const int afterGap = eventRequests;
        QTest::qWait(450);
        QCOMPARE(eventRequests, afterGap);
        advertised = 3;
        QTRY_COMPARE(events.count(), 4);
        QTest::qWait(450);
        QCOMPARE(eventRequests, afterGap + 1);
        // Older compatible services omit the optional status cursor. They must
        // retain periodic event reads rather than silently losing all events.
        legacy = true;
        QTRY_VERIFY(eventRequests > afterGap + 1);
        client.stop();
    }
    void expiredEventsRecoverHistoryWithoutSkippingConcurrentEvents_data() {
        QTest::addColumn<bool>("legacy");
        QTest::newRow("bounded-pages") << false;
        QTest::newRow("legacy-single-page") << true;
    }
    void expiredEventsRecoverHistoryWithoutSkippingConcurrentEvents() {
        QFETCH(bool, legacy);
        HttpFixture server;
        quint64 sequence = 0, advertised = 5;
        int historyReads = 0, eventReads = 0;
        QPointer<QTcpSocket> lastPage;
        QJsonObject lastPageObject;
        server.handler = [&](auto *socket, const QByteArray &path, const QJsonObject &) {
            const QUrlQuery query(QUrl(QString::fromUtf8(path)));
            if (path.startsWith("/api/v1/status")) {
                auto status = statusObject(++sequence, "boot");
                status["executionId"] = "execution"; status["lastEventSeq"] = QString::number(advertised);
                HttpFixture::reply(socket, status);
            } else if (path.startsWith("/api/v1/events")) {
                ++eventReads;
                if (query.queryItemValue("after") == "0") HttpFixture::reply(socket, {{"code", "event_cursor_expired"}}, 410);
                else {
                    QCOMPARE(query.queryItemValue("after"), QString("5"));
                    HttpFixture::reply(socket, {{"apiVersion", 1}, {"bootId", "boot"}, {"events", QJsonArray{QJsonObject{{"seq", "6"}}}}});
                }
            } else if (path.startsWith("/api/v1/executions/execution")) {
                ++historyReads;
                QCOMPARE(query.queryItemValue("limit"), QString("100"));
                QJsonObject record{{"apiVersion", 1}, {"executionId", "execution"},
                    {"events", QJsonArray{QJsonObject{{"seq", "1"}}, QJsonObject{{"seq", legacy ? "5" : "2"}}}}};
                advertised = 6; // A new live event appears while history is read.
                if (legacy) { HttpFixture::reply(socket, record); return; }
                record["historyBootId"] = "boot"; record["historyLastSeq"] = "5";
                if (query.queryItemValue("cursor").isEmpty()) {
                    record["hasMore"] = true; record["nextCursor"] = "opaque-page-2";
                    HttpFixture::reply(socket, record);
                } else {
                    QCOMPARE(query.queryItemValue("cursor"), QString("opaque-page-2"));
                    record["hasMore"] = false; record["nextCursor"] = QJsonValue::Null;
                    record["events"] = QJsonArray{QJsonObject{{"seq", "3"}}, QJsonObject{{"seq", "5"}}};
                    lastPage = socket; lastPageObject = record;
                }
            } else HttpFixture::reply(socket, {{"apiVersion", 1}, {"bootId", "boot"}, {"ready", true}});
        };
        TrackingClient client;
        QSignalSpy pages(&client, &TrackingClient::executionReceived), resync(&client, &TrackingClient::eventsResynchronized);
        QSignalSpy events(&client, &TrackingClient::eventReceived);
        client.configure(server.url(), {}, 2000); client.start();
        if (!legacy) {
            QTRY_VERIFY(lastPage);
            QCOMPARE(pages.count(), 1); QCOMPARE(resync.count(), 0); QCOMPARE(eventReads, 1);
            HttpFixture::reply(lastPage, lastPageObject);
        }
        QTRY_COMPARE(resync.count(), 1);
        QTRY_COMPARE(events.count(), 1);
        QCOMPARE(events.first().at(0).toJsonObject().value("seq").toString(), QString("6"));
        QCOMPARE(pages.count(), legacy ? 1 : 2);
        QCOMPARE(historyReads, legacy ? 1 : 2);
        QCOMPARE(eventReads, 2);
        client.stop();
    }
    void historyRecoveryRejectsRepeatedCursorAndStopsAfterThreeAttempts() {
        HttpFixture server;
        quint64 sequence = 0;
        int eventReads = 0, historyReads = 0;
        server.handler = [&](auto *socket, const QByteArray &path, const QJsonObject &) {
            const QUrlQuery query(QUrl(QString::fromUtf8(path)));
            if (path.startsWith("/api/v1/status")) {
                auto status = statusObject(++sequence, "boot");
                status["executionId"] = "execution"; status["lastEventSeq"] = "3";
                HttpFixture::reply(socket, status);
            } else if (path.startsWith("/api/v1/events")) {
                ++eventReads; QCOMPARE(query.queryItemValue("after"), QString("0"));
                HttpFixture::reply(socket, {{"code", "event_cursor_expired"}}, 410);
            } else if (path.startsWith("/api/v1/executions/")) {
                ++historyReads;
                HttpFixture::reply(socket, {{"apiVersion", 1}, {"executionId", "execution"},
                    {"historyBootId", "boot"}, {"historyLastSeq", "3"}, {"hasMore", true}, {"nextCursor", "repeated-cursor"},
                    {"events", QJsonArray{QJsonObject{{"seq", query.queryItemValue("cursor").isEmpty() ? "1" : "2"}}}}});
            } else HttpFixture::reply(socket, {{"apiVersion", 1}, {"bootId", "boot"}, {"ready", true}});
        };
        TrackingClient client;
        QSignalSpy errors(&client, &TrackingClient::errorOccurred), resync(&client, &TrackingClient::eventsResynchronized);
        client.configure(server.url(), {}, 1000); client.start();
        QTRY_VERIFY_WITH_TIMEOUT(errors.count() >= 3, 14000);
        QCOMPARE(errors.last().at(0).toString(), QString("execution_history_recovery_exhausted"));
        QCOMPARE(eventReads, 3); QCOMPARE(historyReads, 6); QCOMPARE(resync.count(), 0);
        QTest::qWait(5300);
        QCOMPARE(eventReads, 3); QCOMPARE(historyReads, 6); QCOMPARE(resync.count(), 0);
        client.stop();
    }
    void historyRecoveryIgnoresPagesFromPreviousExecution() {
        HttpFixture server;
        quint64 sequence = 0;
        QString execution = "old-execution";
        QPointer<QTcpSocket> oldPage;
        server.handler = [&](auto *socket, const QByteArray &path, const QJsonObject &) {
            if (path.startsWith("/api/v1/status")) {
                auto status = statusObject(++sequence, "boot");
                status["executionId"] = execution; status["lastEventSeq"] = execution == "old-execution" ? "1" : "0";
                HttpFixture::reply(socket, status);
            } else if (path.startsWith("/api/v1/events")) HttpFixture::reply(socket, {{"code", "event_cursor_expired"}}, 410);
            else if (path.startsWith("/api/v1/executions/")) oldPage = socket;
            else HttpFixture::reply(socket, {{"apiVersion", 1}, {"bootId", "boot"}, {"ready", true}});
        };
        TrackingClient client;
        QSignalSpy pages(&client, &TrackingClient::executionReceived), resync(&client, &TrackingClient::eventsResynchronized);
        client.configure(server.url(), {}, 2000); client.start();
        QTRY_VERIFY(oldPage);
        execution = "new-execution";
        QTRY_COMPARE(client.snapshot().executionId, execution);
        HttpFixture::reply(oldPage, {{"apiVersion", 1}, {"executionId", "old-execution"}, {"historyBootId", "boot"},
            {"historyLastSeq", "1"}, {"hasMore", false}, {"events", QJsonArray{QJsonObject{{"seq", "1"}}}}});
        QTest::qWait(250);
        QCOMPARE(pages.count(), 0); QCOMPARE(resync.count(), 0);
        client.stop();
    }

    void reopenedClientRequiresExplicitConfirmedRemotePlan_data() {
        QTest::addColumn<bool>("largePlan");
        QTest::newRow("small-plan") << false;
        QTest::newRow("plan-over-one-mib") << true;
    }
    void reopenedClientRequiresExplicitConfirmedRemotePlan() {
        QFETCH(bool, largePlan);
        HttpFixture server;
        quint64 seq = 0;
        int resumes = 0;
        QString boot = "boot";
        const TrackingContext context{"map", "origin", "transform", "cal", "profile", 1};
        QJsonArray steps{QJsonObject{{"type", "wait"}, {"stepId", "wait"},
                                    {"completion", QJsonObject{{"type", "timer"}, {"durationMs", 100}}}}};
        if (largePlan) {
            steps = {};
            for (int section = 0; section < 5; ++section) {
                QJsonArray points;
                for (int point = 0; point < 10000; ++point)
                    points.append(QJsonArray{.123456789 + (section * 9999 + point) / 100000.0, .987654321});
                steps.append(QJsonObject{{"type", "follow_path"}, {"stepId", QString::number(section)},
                    {"points", points}, {"speedLimit", .2}, {"goalToleranceMeters", .03},
                    {"safetyProfileId", "open"}, {"endBehavior", "stop"}});
            }
        }
        const QJsonObject plan{{"schemaVersion", 1}, {"taskId", "task"}, {"revision", 1},
                              {"context", context.toJson()}, {"repeat", QJsonObject{{"mode", "count"}, {"count", 1}}},
                              {"steps", steps}};
        QJsonObject record{{"apiVersion", 1}, {"taskId", "task"}, {"revision", 1}, {"plan", plan},
                           {"state", "Ready"}, {"planHash", "fnv1a64:123456789abcdef0"}, {"preparedStepCount", steps.size()}};
        if (largePlan) {
            const auto responseBytes = QJsonDocument(record).toJson(QJsonDocument::Compact).size();
            QVERIFY(responseBytes > 1024 * 1024);
            QVERIFY(responseBytes < 8 * 1024 * 1024);
        }
        server.handler = [&](auto *socket, const QByteArray &path, const QJsonObject &body) {
            if (path.startsWith("/api/v1/status")) {
                auto state = statusObject(++seq, boot); state["state"] = "Paused"; state["executionId"] = "run";
                HttpFixture::reply(socket, state);
            } else if (path.endsWith("/sessions"))
                HttpFixture::reply(socket, {{"apiVersion", 1}, {"bootId", boot}, {"sessionId", "session"}, {"sessionToken", "token"}, {"expiresInMs", 1000}});
            else if (path.endsWith("/heartbeat"))
                HttpFixture::reply(socket, {{"apiVersion", 1}, {"bootId", boot}, {"sessionId", "session"}, {"stateVersion", "1"}, {"motionPermit", "permit"}, {"permitValidForMs", 500}});
            else if (path.endsWith("/health"))
                HttpFixture::reply(socket, {{"apiVersion", 1}, {"bootId", boot}, {"ready", true}});
            else if (path.startsWith("/api/v1/tasks/")) HttpFixture::reply(socket, record);
            else if (path.endsWith("/control")) {
                if (body["cmd"].toString() == "resume") ++resumes;
                HttpFixture::reply(socket, {{"commandId", "resume"}}, 202);
            } else if (path.startsWith("/api/v1/commands")) HttpFixture::reply(socket, {{"state", "applied"}});
            else HttpFixture::reply(socket, {{"apiVersion", 1}, {"bootId", boot}, {"events", QJsonArray{}}});
        };
        TrackingClient client;
        client.configure(server.url(), {}); client.start();
        QTRY_VERIFY(client.fresh()); client.acquireSession(); QTRY_VERIFY(client.hasSession());
        QSignalSpy reads(&client, &TrackingClient::taskReceived);
        client.readTask("task", 1); QTRY_COMPARE(reads.count(), 1);
        QVERIFY(client.preparedPlan().isEmpty());
        QVERIFY(!client.control("resume"));
        QVERIFY(!client.confirmReadTask(record, TrackingContext{}));
        auto tampered = record; tampered["planHash"] = "fnv1a64:0000000000000000";
        QVERIFY(!client.confirmReadTask(tampered, context));
        QVERIFY(client.confirmReadTask(record, context));
        QCOMPARE(client.preparedPlan(), plan);
        QCOMPARE(resumes, 0);
        QTRY_VERIFY(client.motionReady());
        QVERIFY(client.control("resume")); QTRY_COMPARE(resumes, 1);
        client.stop();
        QVERIFY(!client.confirmReadTask(record, context));
        client.start(); QTRY_VERIFY(client.fresh()); client.acquireSession(); QTRY_VERIFY(client.hasSession());
        QVERIFY(client.preparedPlan().isEmpty());
        QVERIFY(!client.confirmReadTask(record, context));
        client.readTask("task", 1); QTRY_COMPARE(reads.count(), 2);
        QVERIFY(client.confirmReadTask(record, context));
        boot = "new-boot"; QTRY_COMPARE(client.snapshot().bootId, boot);
        QVERIFY(!client.confirmReadTask(record, context));
        client.stop();
    }
    void degradedFreshPoseRemainsUsable() {
        HttpFixture server;
        quint64 seq = 0;
        bool publish = true;
        server.handler = [&](auto *socket, const auto &, const auto &) {
            HttpFixture::stream(socket, [&] {
            auto object = poseObject(QString::number(publish ? ++seq : seq));
            object["stateAgeMs"] = 220;
            object["positionObservationAgeMs"] = 8000;
            object["headingObservationAgeMs"] = 8000;
            object["positionReliable"] = false;
            object["headingReliable"] = false;
            object["localizationMode"] = "degraded";
            return object;
            });
        };
        PoseClient client;
        client.configure(server.url(), {});
        client.start();
        QTRY_VERIFY_WITH_TIMEOUT(client.fresh(), 500);
        QCOMPARE(client.snapshot().mode, QString("degraded"));
        publish = false;
        QTest::qWait(420);
        QVERIFY(!client.fresh());
        client.stop();
    }
    void poseStreamTracks50HzSourceAndAuthorizes() {
        HttpFixture server;
        quint64 sent = 0;
        int requests = 0;
        QTimer *source = nullptr;
        server.handler = [&](auto *socket, const QByteArray &path, const QJsonObject &) {
            ++requests;
            QCOMPARE(path, QByteArray("/api/v1/pose/stream"));
            QCOMPARE(HttpFixture::requestHeader(socket, "Authorization"), QByteArray("Bearer stream-secret"));
            source = HttpFixture::stream(socket, [&] {
                if (sent == 60) {
                    source->stop();
                    return QJsonObject{};
                }
                auto object = poseObject(QString::number(++sent));
                object["pose"] = QJsonObject{{"x", 1.234}, {"y", -2.345}, {"yaw", .456}};
                return object;
            });
        };
        PoseClient client;
        client.configure(server.url(), "stream-secret", 500);
        QSignalSpy received(&client, &PoseClient::poseChanged);
        QElapsedTimer elapsed;
        elapsed.start();
        client.start();
        QTRY_COMPARE_WITH_TIMEOUT(client.snapshot().seq, quint64(60), 1800);
        QVERIFY2(elapsed.elapsed() < 1800, "50 Hz source was throttled by the client's status timer");
        QCOMPARE(requests, 1);
        // readyRead may contain several validated source frames. The display
        // publishes the latest frame in that burst, so its signal count is
        // independent of the operating system's TCP delivery boundaries.
        QVERIFY(!received.isEmpty());
        QVERIFY(received.size() <= 60);
        quint64 previousSequence = 0;
        for (int index = 0; index < received.size(); ++index) {
            const auto pose = qvariant_cast<ControlPoseSnapshot>(received[index][0]);
            QVERIFY(pose.seq > previousSequence && pose.seq <= 60);
            previousSequence = pose.seq;
            QCOMPARE(pose.position, QPointF(1.234, -2.345));
            QCOMPARE(pose.yaw, .456);
            QVERIFY(pose.validForControl);
        }
        QCOMPARE(previousSequence, quint64(60));
        QVERIFY(client.fresh()); // Stream lifetime must not become transport age.
        client.stop();
    }
    void poseStreamSlowHeadersDoNotAgeEveryFutureSample() {
        HttpFixture server;
        quint64 seq = 0;
        int requests = 0;
        server.handler = [&](auto *socket, const QByteArray &path, const QJsonObject &) {
            ++requests;
            QCOMPARE(path, QByteArray("/api/v1/pose/stream"));
            // Establishment exceeds the state freshness window, then the
            // source starts sending current samples on this same connection.
            QTimer::singleShot(450, socket, [&, socket] {
                HttpFixture::stream(socket, [&] { return poseObject(QString::number(++seq)); });
            });
        };
        PoseClient client;
        client.configure(server.url(), {}, 1000);
        QSignalSpy received(&client, &PoseClient::poseChanged);
        QSignalSpy errors(&client, &PoseClient::errorOccurred);
        QElapsedTimer elapsed;
        elapsed.start();
        client.start();
        QTRY_VERIFY_WITH_TIMEOUT(received.size() >= 20, 1600);
        QVERIFY(elapsed.elapsed() >= 800);
        QCOMPARE(requests, 1);
        QCOMPARE(errors.size(), 0);
        QVERIFY(client.fresh());
        for (const auto &arguments : received) {
            const auto pose = qvariant_cast<ControlPoseSnapshot>(arguments[0]);
            QVERIFY(pose.validForControl);
            QVERIFY(pose.stateAgeMs < 150);
        }
        client.stop();
    }
    void poseStreamQueuedSamplesAgeAndCurrentSamplesRecoverWithoutClockSync_data() {
        QTest::addColumn<bool>("receipt");
        QTest::newRow("source-time") << false;
        QTest::newRow("monotonic-http-receipt") << true;
    }
    void poseStreamQueuedSamplesAgeAndCurrentSamplesRecoverWithoutClockSync() {
        QFETCH(bool, receipt);
        HttpFixture server;
        QPointer<QTcpSocket> stream;
        QElapsedTimer sourceClock;
        constexpr double serverEpoch = 12345.0; // Deliberately unrelated to the client's wall clock.
        server.handler = [&](auto *socket, const QByteArray &, const QJsonObject &) {
            stream = socket;
            HttpFixture::streamHeaders(socket);
            auto pose = poseObject();
            pose["sendTime"] = serverEpoch;
            if (receipt) {
                pose["freshnessBasis"] = "http_receipt";
                pose["sendMonotonicSeconds"] = 0.0;
            }
            sourceClock.start();
            HttpFixture::chunk(socket, QJsonDocument(pose).toJson(QJsonDocument::Compact) + '\n');
        };
        PoseClient client;
        client.configure(server.url(), {}, 1500);
        QSignalSpy received(&client, &PoseClient::poseChanged);
        client.start();
        QTRY_VERIFY(client.fresh());
        // The next frame represents a sample queued shortly after the first,
        // but delivered after a transport/UI pause longer than 350 ms.
        QTest::qWait(450);
        auto queued = poseObject("2");
        queued["sendTime"] = serverEpoch + .02;
        if (receipt) {
            queued["freshnessBasis"] = "http_receipt";
            queued["sendMonotonicSeconds"] = .02;
        }
        queued["pose"] = QJsonObject{{"x", 1.234}, {"y", -2.345}, {"yaw", .456}};
        HttpFixture::chunk(stream, QJsonDocument(queued).toJson(QJsonDocument::Compact) + '\n');
        QTRY_COMPARE(received.size(), 2);
        const auto old = qvariant_cast<ControlPoseSnapshot>(received[1][0]);
        QVERIFY(old.stateAgeMs > 350);
        QVERIFY(!old.validForControl);
        QVERIFY(!client.fresh());
        QCOMPARE(old.position, QPointF(1.234, -2.345));
        QCOMPARE(old.yaw, .456);
        // As the queue drains, a sample sent now becomes fresh immediately,
        // without reconnecting and without dropping any per-sample signal.
        auto current = queued;
        current["seq"] = "3";
        current["sendTime"] = serverEpoch + sourceClock.elapsed() / 1000.0;
        if (receipt) current["sendMonotonicSeconds"] = sourceClock.elapsed() / 1000.0;
        HttpFixture::chunk(stream, QJsonDocument(current).toJson(QJsonDocument::Compact) + '\n');
        QTRY_COMPARE(received.size(), 3);
        const auto recovered = qvariant_cast<ControlPoseSnapshot>(received[2][0]);
        QVERIFY(recovered.stateAgeMs < 150);
        QVERIFY(recovered.validForControl);
        QVERIFY(client.fresh());
        QCOMPARE(recovered.position, old.position);
        QCOMPARE(recovered.yaw, old.yaw);
        client.stop();
    }
    void receiptPoseWallClockAdjustmentsDoNotBreakFreshStream() {
        HttpFixture server;
        quint64 sequence = 0;
        QElapsedTimer sourceClock;
        sourceClock.start();
        server.handler = [&](auto *socket, const QByteArray &, const QJsonObject &) {
            HttpFixture::stream(socket, [&] {
                auto object = poseObject(QString::number(++sequence));
                object["freshnessBasis"] = "http_receipt";
                object["sendMonotonicSeconds"] = sourceClock.elapsed() / 1000.0;
                object["sendTime"] = sequence < 10 ? 10000.0 : sequence < 20 ? 9000.0 : 11000.0;
                return object;
            });
        };
        PoseClient client;
        client.configure(server.url(), {}, 1500);
        QSignalSpy received(&client, &PoseClient::poseChanged), errors(&client, &PoseClient::errorOccurred);
        client.start();
        QTRY_VERIFY(received.size() >= 30);
        QVERIFY(client.fresh());
        QCOMPARE(errors.size(), 0);
        QCOMPARE(client.snapshot().freshnessBasis, "http_receipt");
        QVERIFY(client.snapshot().sendMonotonicSeconds.has_value());
        client.stop();
    }
    void invalidReceiptMonotonicTimeInvalidatesStream_data() {
        QTest::addColumn<int>("scenario");
        QTest::addColumn<QString>("error");
        QTest::newRow("missing") << 0 << QString("pose_stream_monotonic_time_invalid");
        QTest::newRow("negative") << 1 << QString("pose_stream_monotonic_time_invalid");
        QTest::newRow("rewound") << 2 << QString("pose_stream_clock_rewound");
    }
    void invalidReceiptMonotonicTimeInvalidatesStream() {
        QFETCH(int, scenario);
        QFETCH(QString, error);
        HttpFixture server;
        QPointer<QTcpSocket> stream;
        server.handler = [&](auto *socket, const QByteArray &, const QJsonObject &) {
            stream = socket;
            HttpFixture::streamHeaders(socket);
            auto first = poseObject();
            first["freshnessBasis"] = "http_receipt";
            first["sendMonotonicSeconds"] = 100.0;
            HttpFixture::chunk(socket, QJsonDocument(first).toJson(QJsonDocument::Compact) + '\n');
        };
        PoseClient client;
        client.configure(server.url(), {}, 1500);
        QSignalSpy errors(&client, &PoseClient::errorOccurred);
        client.start();
        QTRY_VERIFY(client.fresh());
        auto invalid = poseObject("2");
        invalid["freshnessBasis"] = "http_receipt";
        if (scenario == 1) invalid["sendMonotonicSeconds"] = -1.0;
        if (scenario == 2) invalid["sendMonotonicSeconds"] = 99.0;
        HttpFixture::chunk(stream, QJsonDocument(invalid).toJson(QJsonDocument::Compact) + '\n');
        QTRY_COMPARE(errors.size(), 1);
        QCOMPARE(errors[0][0].toString(), error);
        QVERIFY(!client.fresh());
        client.stop();
    }
    void poseStreamInvalidOrRewoundSendTimeInvalidatesConnection_data() {
        QTest::addColumn<int>("scenario");
        QTest::addColumn<QString>("error");
        QTest::newRow("missing-time") << 0 << QString("pose_stream_send_time_invalid");
        QTest::newRow("non-numeric-time") << 1 << QString("pose_stream_send_time_invalid");
        QTest::newRow("time-rewound") << 2 << QString("pose_stream_clock_rewound");
    }
    void poseStreamInvalidOrRewoundSendTimeInvalidatesConnection() {
        QFETCH(int, scenario);
        QFETCH(QString, error);
        HttpFixture server;
        QPointer<QTcpSocket> stream;
        server.handler = [&](auto *socket, const QByteArray &, const QJsonObject &) {
            stream = socket;
            HttpFixture::streamHeaders(socket);
            auto first = poseObject();
            first["sendTime"] = 10000.0;
            HttpFixture::chunk(socket, QJsonDocument(first).toJson(QJsonDocument::Compact) + '\n');
        };
        PoseClient client;
        client.configure(server.url(), {}, 1000);
        QSignalSpy received(&client, &PoseClient::poseChanged);
        QSignalSpy errors(&client, &PoseClient::errorOccurred);
        client.start();
        QTRY_VERIFY(client.fresh());
        auto invalid = poseObject("2");
        if (scenario == 0)
            invalid.remove("sendTime");
        else if (scenario == 1)
            invalid["sendTime"] = "10000.02";
        else
            invalid["sendTime"] = 9999.0;
        HttpFixture::chunk(stream, QJsonDocument(invalid).toJson(QJsonDocument::Compact) + '\n');
        QTRY_COMPARE(errors.size(), 1);
        QCOMPARE(errors.first().first().toString(), error);
        QCOMPARE(received.size(), 1);
        QVERIFY(!client.fresh());
        client.stop();
    }
    void poseStreamHandlesFragmentedAndCoalescedLinesWithoutLifetimeLimit() {
        HttpFixture server;
        QPointer<QTcpSocket> stream;
        int requests = 0;
        server.handler = [&](auto *socket, const QByteArray &path, const QJsonObject &) {
            ++requests;
            QCOMPARE(path, QByteArray("/api/v1/pose/stream"));
            stream = socket;
            HttpFixture::streamHeaders(socket);
            const auto first = QJsonDocument(poseObject()).toJson(QJsonDocument::Compact) + "\r\n";
            HttpFixture::chunk(socket, first.left(17));
            QTimer::singleShot(10, socket, [socket, first] {
                QByteArray batch = first.mid(17);
                for (int seq = 2; seq <= 900; ++seq) {
                    auto pose = poseObject(QString::number(seq));
                    pose["diagnosticPadding"] = QString(2048, QLatin1Char('x'));
                    batch += QJsonDocument(pose).toJson(QJsonDocument::Compact) + '\n';
                }
                QVERIFY(batch.size() > 1024 * 1024);
                HttpFixture::chunk(socket, batch);
            });
        };
        PoseClient client;
        client.configure(server.url(), {}, 1000);
        QSignalSpy received(&client, &PoseClient::poseChanged);
        QSignalSpy errors(&client, &PoseClient::errorOccurred);
        client.start();
        QTRY_COMPARE_WITH_TIMEOUT(client.snapshot().seq, quint64(900), 5000);
        QVERIFY(!received.isEmpty());
        QVERIFY(received.size() <= 900);
        quint64 lastDisplayedSequence = 0;
        for (int index = 0; index < received.size(); ++index) {
            const auto pose = qvariant_cast<ControlPoseSnapshot>(received[index][0]);
            QVERIFY(pose.seq > lastDisplayedSequence);
            QVERIFY(pose.seq <= 900);
            lastDisplayedSequence = pose.seq;
            QCOMPARE(pose.position, QPointF());
            QCOMPARE(pose.yaw, 0.0);
        }
        QCOMPARE(lastDisplayedSequence, quint64(900));
        QCOMPARE(errors.size(), 0);
        QCOMPARE(requests, 1);
        QVERIFY(stream);
        // Parsing the large queued batch can exceed freshness on a slow host.
        // A newly produced sample must still arrive fresh on this same stream.
        const auto displayedBeforeNewSample = received.size();
        HttpFixture::chunk(stream, QJsonDocument(poseObject("901")).toJson(QJsonDocument::Compact) + '\n');
        QTRY_COMPARE_WITH_TIMEOUT(client.snapshot().seq, quint64(901), 500);
        QCOMPARE(received.size(), displayedBeforeNewSample + 1);
        QVERIFY(client.fresh());
        QCOMPARE(errors.size(), 0);
        QCOMPARE(requests, 1);
        client.stop();
    }
    void poseStreamRejectsInvalidResponsesWithoutPollingFallback_data() {
        QTest::addColumn<int>("scenario");
        QTest::addColumn<QString>("error");
        QTest::newRow("unsupported-service") << 0 << QString("HTTP 404");
        QTest::newRow("ordinary-json") << 1 << QString("pose_stream_content_type");
        QTest::newRow("malformed-line") << 2 << QString("pose_stream_invalid_json");
        QTest::newRow("oversized-line") << 3 << QString("pose_stream_line_too_large");
    }
    void poseStreamRejectsInvalidResponsesWithoutPollingFallback() {
        QFETCH(int, scenario);
        QFETCH(QString, error);
        HttpFixture server;
        int requests = 0;
        server.handler = [&](auto *socket, const QByteArray &path, const QJsonObject &) {
            ++requests;
            QCOMPARE(path, QByteArray("/api/v1/pose/stream"));
            if (scenario < 2)
                HttpFixture::reply(socket, poseObject(), scenario == 0 ? 404 : 200);
            else {
                HttpFixture::streamHeaders(socket);
                HttpFixture::chunk(socket, scenario == 2 ? QByteArray("{broken}\n")
                                                        : QByteArray(64 * 1024 + 1, 'x'));
            }
        };
        PoseClient client;
        client.configure(server.url(), {}, 500);
        QSignalSpy errors(&client, &PoseClient::errorOccurred);
        QSignalSpy received(&client, &PoseClient::poseChanged);
        client.start();
        QTRY_VERIFY(!errors.isEmpty());
        QVERIFY2(errors.first().first().toString().contains(error), qPrintable(errors.first().first().toString()));
        QVERIFY(!client.fresh());
        QCOMPARE(received.size(), 0);
        client.stop();
        const auto stoppedRequests = requests;
        QTest::qWait(250);
        QCOMPARE(requests, stoppedRequests);
    }
    void poseStreamDisconnectReconnectDoesNotReplayAndStopCancels() {
        HttpFixture server;
        QPointer<QTcpSocket> stream;
        quint64 seq = 10;
        int requests = 0;
        server.handler = [&](auto *socket, const QByteArray &path, const QJsonObject &) {
            QCOMPARE(path, QByteArray("/api/v1/pose/stream"));
            ++requests;
            stream = socket;
            HttpFixture::stream(socket, [&] { return poseObject(QString::number(seq)); });
        };
        PoseClient client;
        client.configure(server.url(), {}, 1000);
        QSignalSpy received(&client, &PoseClient::poseChanged);
        client.start();
        QTRY_VERIFY(client.fresh());
        stream->abort();
        QTRY_VERIFY(!client.fresh());
        QTRY_COMPARE(requests, 2);
        QTest::qWait(80);
        QCOMPARE(received.size(), 1);
        QVERIFY(!client.fresh());
        seq = 11;
        QTRY_VERIFY(client.fresh());
        QCOMPARE(received.size(), 2);
        client.stop();
        seq = 12;
        QTest::qWait(300);
        QCOMPARE(received.size(), 2);
        QCOMPARE(requests, 2);
        QVERIFY(!client.fresh());
    }
    void poseStreamSourceStallBecomesStaleThenTimesOutAndBacksOff() {
        HttpFixture server;
        int requests = 0;
        server.handler = [&](auto *socket, const QByteArray &, const QJsonObject &) {
            ++requests;
            HttpFixture::streamHeaders(socket);
            HttpFixture::chunk(socket, QJsonDocument(poseObject()).toJson(QJsonDocument::Compact) + '\n');
        };
        PoseClient client;
        client.configure(server.url(), {}, 600);
        QSignalSpy errors(&client, &PoseClient::errorOccurred);
        client.start();
        QTRY_VERIFY(client.fresh());
        QTest::qWait(390);
        QVERIFY(!client.fresh());
        QCOMPARE(errors.size(), 0);
        QTRY_COMPARE(errors.size(), 1);
        QCOMPARE(errors.first().first().toString(), QString("pose_stream_timeout"));
        QCOMPARE(requests, 1);
        QTest::qWait(100);
        QCOMPARE(requests, 1);
        QTRY_COMPARE(requests, 2);
        QVERIFY(!client.fresh()); // Reconnecting to the same sample cannot refresh it.
        client.stop();
    }
    void manualPublishesToLegacySocketWithoutControlFeedback() {
        QWebSocketServer server(QStringLiteral("legacy"), QWebSocketServer::NonSecureMode);
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QJsonArray packets;
        connect(&server, &QWebSocketServer::newConnection, this, [&] {
            auto *socket = server.nextPendingConnection();
            connect(socket, &QWebSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QWebSocket::textMessageReceived, this, [&](const QString &text) {
                packets.append(QJsonDocument::fromJson(text.toUtf8()).object());
            });
        });
        ChassisClient client;
        client.setUrl(QUrl(QStringLiteral("ws://127.0.0.1:%1").arg(server.serverPort())));
        client.connectToHost();
        QTRY_VERIFY(client.isConnected());
        client.sendVelocityCommand(.2, -.1);
        QTRY_VERIFY_WITH_TIMEOUT(!packets.isEmpty() &&
            packets.last().toObject()["packet"].toObject()["region"] == "cmd_vel", 500);
        QCOMPARE(packets.last().toObject()["msg"].toObject(),
                 (QJsonObject{{"xvel", .2}, {"yvel", 0.0}, {"thetavel", -.1}, {"isRemote", true}}));
        client.disconnectFromHost();
    }
    void captureCannotConsumeResultFromRestartedFusion() {
        HttpFixture server;
        quint64 seq = 0;
        server.handler = [&](auto *socket, const QByteArray &path, const QJsonObject &) {
            if (path == "/api/v1/pose/stream")
                HttpFixture::stream(socket, [&] { return poseObject(QString::number(++seq)); });
            else if (path == "/api/v1/captures")
                HttpFixture::reply(socket,
                                   {{"apiVersion", 1},
                                    {"fusionBootId", "pose-a"},
                                    {"captureId", "capture-1"},
                                    {"state", "running"}},
                                   202);
            else
                HttpFixture::reply(socket, {{"apiVersion", 1},
                                            {"fusionBootId", "pose-b"},
                                            {"captureId", "capture-1"},
                                            {"state", "succeeded"}});
        };
        PoseClient pose;
        pose.configure(server.url(), {}, 500);
        pose.start();
        QTRY_VERIFY(pose.fresh());
        MapFrameBinding binding;
        binding.confirmed = true;
        binding.context = {"map", "origin", "transform", "cal", "profile", 1};
        QSignalSpy captures(&pose, &PoseClient::captureFinished);
        QSignalSpy errors(&pose, &PoseClient::errorOccurred);
        QVERIFY(pose.capture(binding));
        QTRY_COMPARE(errors.size(), 1);
        QCOMPARE(captures.size(), 0);
        pose.stop();
    }
    void photoIntentSurvivesLostResponseAndCoordinatorRestart() {
        HttpFixture server;
        QTemporaryDir journal;
        quint64 seq = 0;
        int photos = 0, acks = 0, ackQueries = 0;
        QHash<QString, QString> acceptedRequests;
        bool waiting = false;
        const QJsonObject event{{"eventId", "execution/8"}, {"executionId", "execution"},
                                {"stepId", "photo"},        {"type", "external_action_requested"},
                                {"reason", "capture"},      {"seq", "1"}};
        server.handler = [&](auto *socket, const QByteArray &path, const QJsonObject &body) {
            if (path.startsWith("/api/v1/status")) {
                auto status = statusObject(++seq, "boot");
                if (waiting) {
                    status["state"] = "Waiting";
                    status["executionId"] = "execution";
                    status["stepId"] = "photo";
                    status["waitingEventId"] = "execution/8";
                    status["lastEventSeq"] = "1";
                }
                HttpFixture::reply(socket, status);
            } else if (path.endsWith("/health"))
                HttpFixture::reply(socket, {{"apiVersion", 1}, {"bootId", "boot"}, {"ready", true}});
            else if (path.endsWith("/configuration"))
                HttpFixture::reply(socket, {{"apiVersion", 1}});
            else if (path.startsWith("/api/v1/events/") && path.endsWith("/ack")) {
                ++acks;
                acceptedRequests.insert(body["requestId"].toString(), "ack");
                HttpFixture::reply(socket, {{"commandId", "ack"}}, 202);
            } else if (path.startsWith("/api/v1/events"))
                HttpFixture::reply(socket, {{"apiVersion", 1},
                                            {"bootId", "boot"},
                                            {"events", waiting ? QJsonArray{event} : QJsonArray{}}});
            else if (path.endsWith("/sessions"))
                HttpFixture::reply(socket, {{"apiVersion", 1},
                                            {"bootId", "boot"},
                                            {"sessionId", "session"},
                                            {"sessionToken", "token"},
                                            {"expiresInMs", 1000}});
            else if (path.endsWith("/heartbeat"))
                HttpFixture::reply(socket, {{"apiVersion", 1},
                                            {"bootId", "boot"},
                                            {"sessionId", "session"},
                                            {"stateVersion", "1"},
                                            {"motionPermit", "permit"},
                                            {"permitValidForMs", 500}});
            else if (path.endsWith("/tasks")) {
                acceptedRequests.insert(body["requestId"].toString(), "prepare");
                HttpFixture::reply(socket, {{"commandId", "prepare"}}, 202);
            } else if (path.startsWith("/api/v1/commands")) {
                if (path == "/api/v1/commands/ack")
                    ++ackQueries;
                const bool discovery = path.startsWith("/api/v1/commands?");
                const auto id =
                    discovery ? acceptedRequests.value(
                                    QUrlQuery(QUrl(QString::fromUtf8(path))).queryItemValue("requestId"))
                              : QString::fromUtf8(path.mid(QByteArray("/api/v1/commands/").size()));
                if (id.isEmpty())
                    HttpFixture::reply(socket, {}, 404);
                else
                    HttpFixture::reply(socket,
                                       {{"state", discovery ? "pending" : "applied"}, {"commandId", id}});
            } else if (path.endsWith("/camera/photo")) {
                ++photos;
                socket->abort();
            } else
                HttpFixture::reply(socket, {}, 404);
        };
        TrackingClient tracking;
        tracking.configure(server.url(), {}, 500);
        tracking.start();
        QTRY_VERIFY(tracking.fresh());
        tracking.acquireSession();
        QTRY_VERIFY(tracking.hasSession());
        QTRY_VERIFY(tracking.configurationFresh());
        QVERIFY(tracking.upload({{"taskId", "task"}, {"revision", 1}}));
        QTRY_VERIFY(!tracking.busy());
        {
            ExternalEventCoordinator coordinator(&tracking);
            coordinator.configure(server.url(), {}, journal.path(), 250);
            waiting = true;
            QTRY_COMPARE(photos, 1);
            QTest::qWait(500);
            QCOMPARE(acks, 0);
            QCOMPARE(photos, 1);
            QTRY_COMPARE(coordinator.manualConfirmationEventId(), QString("execution/8"));
        }
        ExternalEventCoordinator recovered(&tracking);
        recovered.configure(server.url(), {}, journal.path(), 250);
        tracking.eventReceived(event);
        QTest::qWait(500);
        QCOMPARE(photos, 1);
        QCOMPARE(acks, 0);
        // Operator resolution requires a current status and live session;
        // the observation wait above does not synchronize either condition.
        QTRY_VERIFY2(tracking.fresh() && tracking.hasSession() &&
                         tracking.snapshot().waitingEventId == "execution/8" &&
                         tracking.snapshot().state == "Waiting" &&
                         tracking.snapshot().executionId == "execution" &&
                         tracking.snapshot().stepId == "photo",
                     qPrintable(QString("fresh=%1 session=%2 state=%3 event=%4 execution=%5 step=%6")
                                    .arg(tracking.fresh()).arg(tracking.hasSession())
                                    .arg(tracking.snapshot().state, tracking.snapshot().waitingEventId,
                                         tracking.snapshot().executionId, tracking.snapshot().stepId)));
        QVERIFY(!recovered.resolveCurrent("execution/old", true));
        QTRY_COMPARE(recovered.manualConfirmationEventId(), QString("execution/8"));
        QVERIFY(recovered.resolveCurrent("execution/8", true));
        QVERIFY(recovered.manualConfirmationEventId().isEmpty());
        QTRY_COMPARE(acks, 1);
        QTRY_COMPARE(ackQueries, 1);
        QVERIFY(!recovered.resolveCurrent("execution/8", false));
        QTest::qWait(400);
        QCOMPARE(photos, 1);
        QCOMPARE(acks, 1);
        tracking.stop();
    }
    void manualPhotoConfirmationIsContextualAndNeverReplaysCapture() {
        HttpFixture server;
        QTemporaryDir journal;
        quint64 sequence = 0;
        QString eventId = "execution/unknown", state = "Waiting";
        bool offline = false;
        int photos = 0, acks = 0;
        QHash<QString, QString> requests;
        server.handler = [&](auto *socket, const QByteArray &path, const QJsonObject &body) {
            if (path.startsWith("/api/v1/status")) {
                auto status = statusObject(++sequence, "boot");
                status["state"] = state;
                status["executionId"] = "execution";
                status["stepId"] = "photo";
                status["waitingEventId"] = eventId;
                HttpFixture::reply(socket, status, offline ? 503 : 200);
            } else if (path.endsWith("/sessions"))
                HttpFixture::reply(socket, {{"apiVersion", 1}, {"bootId", "boot"},
                    {"sessionId", "session"}, {"sessionToken", "token"}, {"expiresInMs", 3000}});
            else if (path.endsWith("/heartbeat"))
                HttpFixture::reply(socket, {{"apiVersion", 1}, {"bootId", "boot"},
                    {"sessionId", "session"}, {"stateVersion", "1"},
                    {"motionPermit", "permit"}, {"permitValidForMs", 500}});
            else if (path.startsWith("/api/v1/events/") && path.endsWith("/ack")) {
                ++acks;
                const auto request = body["requestId"].toString();
                requests[request] = request;
                HttpFixture::reply(socket, {{"commandId", request}}, 202);
            } else if (path.startsWith("/api/v1/commands")) {
                const auto request = path.startsWith("/api/v1/commands?")
                    ? QUrlQuery(QUrl(QString::fromUtf8(path))).queryItemValue("requestId")
                    : QString::fromUtf8(path.mid(QByteArray("/api/v1/commands/").size()));
                HttpFixture::reply(socket, {{"commandId", request}, {"requestId", request},
                    {"state", "applied"}}, requests.contains(request) ? 200 : 404);
            } else if (path.endsWith("/camera/photo")) {
                ++photos;
                HttpFixture::reply(socket, {{"success", true}, {"path", "unexpected.jpg"}});
            } else
                HttpFixture::reply(socket, {{"apiVersion", 1}, {"bootId", "boot"},
                    {"ready", true}, {"events", QJsonArray{}}});
        };
        const auto store = [&](const QString &id, const QString &savedState) {
            const auto name = QString::fromLatin1(QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha256).toHex()) + ".json";
            QFile file(QDir(journal.path()).filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QVERIFY(file.write(QJsonDocument(QJsonObject{{"eventId", id}, {"state", savedState}}).toJson()) > 0);
        };
        TrackingClient tracking;
        ExternalEventCoordinator coordinator(&tracking);
        QSignalSpy changes(&coordinator, &ExternalEventCoordinator::manualConfirmationChanged);
        coordinator.configure(server.url(), {}, journal.path(), 250);
        tracking.configure(server.url(), {}, 250);
        tracking.start();
        // Reopening has no prepared plan or session. Offer the contextual action,
        // but submitting a resolution still needs the explicit operation session.
        QTRY_COMPARE(coordinator.manualConfirmationEventId(), eventId);
        QVERIFY(!tracking.hasSession());
        QVERIFY(!coordinator.resolveCurrent(eventId, true));
        const int unchanged = changes.size();
        QTest::qWait(450);
        QCOMPARE(changes.size(), unchanged);
        QCOMPARE(photos, 0);
        state = "Paused";
        QTRY_COMPARE(tracking.snapshot().state, state);
        QCOMPARE(coordinator.manualConfirmationEventId(), eventId);
        eventId = "execution/other";
        QTRY_COMPARE(coordinator.manualConfirmationEventId(), eventId);
        QVERIFY(!coordinator.resolveCurrent("execution/unknown", true));
        offline = true;
        QTRY_VERIFY(!tracking.fresh());
        QVERIFY(coordinator.manualConfirmationEventId().isEmpty());
        offline = false;
        QTRY_COMPARE(coordinator.manualConfirmationEventId(), eventId);
        coordinator.configure(server.url(), {}, {}, 250);
        QVERIFY(coordinator.manualConfirmationEventId().isEmpty());
        coordinator.configure(server.url(), {}, journal.path(), 250);
        QTRY_COMPARE(coordinator.manualConfirmationEventId(), eventId);
        tracking.acquireSession();
        QTRY_VERIFY(tracking.hasSession());
        QVERIFY(coordinator.resolveCurrent(eventId, false));
        QVERIFY(coordinator.manualConfirmationEventId().isEmpty());
        QTRY_COMPARE(acks, 1);
        QTRY_VERIFY(!tracking.busy());
        QVERIFY(!coordinator.resolveCurrent(eventId, true));
        QVERIFY(coordinator.manualConfirmationEventId().isEmpty());
        for (const auto &saved : {QString("succeeded"), QString("failed")}) {
            const QString next = "execution/" + saved;
            store(next, saved);
            eventId = next;
            QTRY_COMPARE(tracking.snapshot().waitingEventId, eventId);
            QTest::qWait(250);
            QVERIFY(coordinator.manualConfirmationEventId().isEmpty());
            QVERIFY(!coordinator.resolveCurrent(eventId, true));
        }
        store("execution/intent", "intent");
        eventId = "execution/intent";
        QTRY_COMPARE(coordinator.manualConfirmationEventId(), eventId);
        state = "Completed";
        QTRY_COMPARE(tracking.snapshot().state, state);
        QVERIFY(coordinator.manualConfirmationEventId().isEmpty());
        QCOMPARE(photos, 0);
        tracking.stop();
        QVERIFY(coordinator.manualConfirmationEventId().isEmpty());
    }
    void boundedRequestsAndSeparateSafetyLane() {
        HttpFixture server;
        server.handler = [](auto *socket, const auto &path, const auto &) {
            if (path.endsWith("/stop"))
                HttpFixture::reply(socket, {{"ok", true}});
        };
        JsonHttpClient client;
        client.configure(server.url(), {}, 200);
        int readFinished = 0, stopFinished = 0;
        QString error;
        QVERIFY(client.request("read", "GET", "/slow", {}, {}, {}, [&](const auto &r) {
            ++readFinished;
            error = r.error;
        }));
        QVERIFY(!client.request("read", "GET", "/duplicate", {}, {}, {}, [](const auto &) {}));
        QVERIFY(client.request("safety", "POST", "/stop", {}, {}, {}, [&](const auto &r) {
            if (r.ok())
                ++stopFinished;
        }));
        QTRY_COMPARE(stopFinished, 1);
        QTRY_COMPARE(readFinished, 1);
        QCOMPARE(error, QString("request_timeout"));
        QVERIFY(!client.busy("read"));
        int cancelled = 0;
        QVERIFY(client.request("read", "GET", "/slow", {}, {}, {}, [&](const auto &) { ++cancelled; }));
        client.cancelAll();
        QTest::qWait(250);
        QCOMPARE(cancelled, 0);
    }
    void defaultResponseLimitRejectsOversizeDocument() {
        HttpFixture server;
        server.handler = [](auto *socket, const auto &, const auto &) {
            HttpFixture::reply(socket, {{"payload", QString(1024 * 1024, QLatin1Char('x'))}});
        };
        JsonHttpClient client;
        client.configure(server.url(), {});
        int finished = 0;
        JsonHttpResult result;
        QVERIFY(client.request("read", "GET", "/large", {}, {}, {}, [&](const auto &response) {
            result = response;
            ++finished;
        }));
        QTRY_COMPARE(finished, 1);
        QCOMPARE(result.error, QString("response_too_large"));
        QVERIFY(!result.ok());
        QVERIFY(result.object.isEmpty());
        QVERIFY(!client.busy("read"));
    }
    void responseLimitCannotExceedDocumentMaximum() {
        HttpFixture server;
        server.handler = [](auto *socket, const auto &, const auto &) {
            HttpFixture::reply(socket, {{"payload", QString(16 * 1024 * 1024, QLatin1Char('x'))}});
        };
        JsonHttpClient client;
        client.configure(server.url(), {}, 10000);
        int finished = 0;
        JsonHttpResult result;
        QVERIFY(client.request("read", "GET", "/large", {}, {}, {}, [&](const auto &response) {
            result = response;
            ++finished;
        }, 32 * 1024 * 1024));
        QTRY_COMPARE_WITH_TIMEOUT(finished, 1, 10000);
        QCOMPARE(result.error, QString("response_too_large"));
        QVERIFY(!result.ok());
        QVERIFY(result.object.isEmpty());
        QVERIFY(!client.busy("read"));
    }
    void duplicatePoseDoesNotRefreshAgeAndRebootIsVisible() {
        HttpFixture server;
        auto response = poseObject();
        server.handler = [&](auto *socket, const auto &, const auto &) {
            HttpFixture::stream(socket, [&] { return response; });
        };
        PoseClient client;
        client.configure(server.url(), {});
        QSignalSpy changes(&client, &PoseClient::frameChanged);
        client.start();
        QTRY_VERIFY(client.fresh());
        QTest::qWait(420);
        QVERIFY(!client.fresh());
        response = poseObject("2");
        QTRY_VERIFY(client.fresh());
        response = poseObject("3");
        response["positionObservationAgeMs"] = 900;
        QTRY_COMPARE(client.snapshot().seq, quint64(3));
        QVERIFY(client.fresh());
        response = poseObject("1", "pose-b");
        QTRY_COMPARE(changes.count(), 1);
        QTRY_VERIFY(client.fresh());
        response = poseObject("2", "pose-b");
        response["stateAgeMs"] = "invalid";
        QTRY_VERIFY(!client.fresh());
        client.stop();
    }
    void uncertainStartIsQueriedAndNeverResent() {
        HttpFixture server;
        quint64 seq = 0;
        int acquires = 0, starts = 0, queries = 0, heartbeats = 0;
        bool acceptHeartbeat = true;
        QString boot = "tracking-a";
        server.handler = [&](auto *socket, const QByteArray &path, const QJsonObject &body) {
            if (path.startsWith("/api/v1/status"))
                HttpFixture::reply(socket, statusObject(++seq, boot));
            else if (path.endsWith("/health"))
                HttpFixture::reply(socket, {{"apiVersion", 1}, {"bootId", boot}, {"ready", true}});
            else if (path.endsWith("/configuration"))
                HttpFixture::reply(socket, {{"apiVersion", 1}});
            else if (path.startsWith("/api/v1/events"))
                HttpFixture::reply(socket, {{"apiVersion", 1}, {"bootId", boot}, {"events", QJsonArray{}}});
            else if (path.endsWith("/sessions")) {
                ++acquires;
                HttpFixture::reply(socket, {{"apiVersion", 1},
                                            {"bootId", boot},
                                            {"sessionId", "session"},
                                            {"sessionToken", "token"},
                                            {"expiresInMs", 1000}});
            } else if (path.endsWith("/heartbeat")) {
                ++heartbeats;
                if (acceptHeartbeat)
                    HttpFixture::reply(socket, {{"apiVersion", 1},
                                                {"bootId", boot},
                                                {"sessionId", "session"},
                                                {"stateVersion", "1"},
                                                {"motionPermit", "permit"},
                                                {"permitValidForMs", 500}});
            } else if (path.endsWith("/tasks"))
                HttpFixture::reply(socket, {{"commandId", "prepare"}}, 202);
            else if (path.endsWith("/control")) {
                if (body["cmd"].toString() == "start")
                    ++starts;
                socket->abort();
            } else if (path.startsWith("/api/v1/commands")) {
                ++queries;
                // Prepare has an acknowledged command id; the lost start
                // response is resolved by request id. Keep both replies
                // consistent regardless of which callback arrives first.
                const auto commandId = path == "/api/v1/commands/prepare" ? "prepare" : "known";
                HttpFixture::reply(socket, {{"state", "applied"}, {"commandId", commandId}});
            } else
                HttpFixture::reply(socket, {}, 404);
        };
        TrackingClient client;
        client.configure(server.url(), {}, 250);
        client.start();
        QTRY_VERIFY(client.fresh());
        client.acquireSession();
        QTRY_VERIFY(client.hasSession());
        QTRY_VERIFY(client.configurationFresh());
        QVERIFY(client.upload({{"taskId", "task"}, {"revision", 1}}));
        QTRY_VERIFY(!client.busy());
        QTRY_VERIFY(client.motionReady());
        QVERIFY(client.control("start"));
        QTRY_COMPARE(starts, 1);
        QTRY_VERIFY(!client.busy());
        QVERIFY(queries >= 2);
        QTest::qWait(300);
        QCOMPARE(starts, 1);
        acceptHeartbeat = false;
        QTRY_VERIFY(!client.hasSession());
        QTest::qWait(300);
        QCOMPARE(acquires, 1);
        QVERIFY(!client.control("start"));
        boot = "tracking-b";
        QSignalSpy restarted(&client, &TrackingClient::serverRestarted);
        QTRY_COMPARE(restarted.count(), 1);
        QVERIFY(client.preparedPlan().isEmpty());
        QCOMPARE(acquires, 1);
        QVERIFY(heartbeats > 1);
        client.stop();
    }
    void manualReconnectDoesNotReplayVelocity() {
        QWebSocketServer server("test", QWebSocketServer::NonSecureMode);
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QPointer<QWebSocket> peer;
        int packets = 0;
        connect(&server, &QWebSocketServer::newConnection, this, [&] {
            peer = server.nextPendingConnection();
            connect(peer, &QWebSocket::disconnected, peer, &QObject::deleteLater);
            connect(peer, &QWebSocket::textMessageReceived, this, [&](const QString &) { ++packets; });
        });
        ChassisClient client;
        client.setAutoReconnect(false);
        client.setUrl(QUrl(QString("ws://127.0.0.1:%1").arg(server.serverPort())));
        client.connectToHost();
        QTRY_VERIFY(client.isConnected());
        client.sendVelocityCommand(.2, 0);
        QTRY_COMPARE(packets, 1);
        peer->close();
        QTRY_VERIFY(!client.isConnected());
        client.sendVelocityCommand(.3, 0);
        client.connectToHost();
        QTRY_VERIFY(client.isConnected());
        QTest::qWait(250);
        QCOMPARE(packets, 1);
        client.sendVelocityCommand(.1, 0);
        QTRY_COMPARE(packets, 2);
        client.disconnectFromHost();
    }

};
QTEST_GUILESS_MAIN(IpcClientsTest)
#include "test_ipc_clients.moc"
