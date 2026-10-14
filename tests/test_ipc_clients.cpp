#include "chassisclient.h"
#include "externaleventcoordinator.h"
#include "jsonhttpclient.h"
#include "poseclient.h"
#include "trackingclient.h"
#include <QJsonArray>
#include <QJsonDocument>
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
                    handler(socket, header.split(' ').value(1),
                            QJsonDocument::fromJson(data->mid(end + 4, length)).object());
                });
            }
        });
    }
    QUrl url() const { return QUrl(QString("http://127.0.0.1:%1/api/v1").arg(serverPort())); }
    static void reply(QTcpSocket *socket, const QJsonObject &json, int code = 200) {
        const auto bytes = QJsonDocument(json).toJson(QJsonDocument::Compact);
        socket->write("HTTP/1.1 " + QByteArray::number(code) +
                      " Result\r\nContent-Type: application/json\r\nContent-Length: " +
                      QByteArray::number(bytes.size()) + "\r\nConnection: close\r\n\r\n" + bytes);
        socket->disconnectFromHost();
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

class IpcClientsTest : public QObject {
    Q_OBJECT
  private slots:
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
            auto object = poseObject(QString::number(publish ? ++seq : seq));
            object["stateAgeMs"] = 220;
            object["positionObservationAgeMs"] = 8000;
            object["headingObservationAgeMs"] = 8000;
            object["positionReliable"] = false;
            object["headingReliable"] = false;
            object["localizationMode"] = "degraded";
            HttpFixture::reply(socket, object);
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
            if (path.startsWith("/api/v1/pose"))
                HttpFixture::reply(socket, poseObject(QString::number(++seq)));
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
        }
        ExternalEventCoordinator recovered(&tracking);
        recovered.configure(server.url(), {}, journal.path(), 250);
        tracking.eventReceived(event);
        QTest::qWait(500);
        QCOMPARE(photos, 1);
        QCOMPARE(acks, 0);
        QVERIFY(!recovered.resolveCurrent("execution/old", true));
        QVERIFY(recovered.resolveCurrent("execution/8", true));
        QTRY_COMPARE(acks, 1);
        QTRY_COMPARE(ackQueries, 1);
        QVERIFY(!recovered.resolveCurrent("execution/8", false));
        QTest::qWait(400);
        QCOMPARE(photos, 1);
        QCOMPARE(acks, 1);
        tracking.stop();
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
            HttpFixture::reply(socket, response);
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
                HttpFixture::reply(socket, {{"state", "applied"}, {"commandId", "known"}});
            } else
                HttpFixture::reply(socket, {}, 404);
        };
        TrackingClient client;
        client.configure(server.url(), {}, 250);
        client.start();
        QTRY_VERIFY(client.fresh());
        client.acquireSession();
        QTRY_VERIFY(client.hasSession());
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
