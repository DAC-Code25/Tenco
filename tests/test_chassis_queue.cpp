#include "chassisclient.h"
#include "chassisvelocityqueue.h"
#include "motioncommandarbiter.h"
#include <QJsonDocument>
#include <QPointer>
#include <QTimer>
#include <QWebSocket>
#include <QWebSocketServer>
#include <QtTest>
#include <limits>

namespace {
class ChassisFixture : public QWebSocketServer {
public:
    QPointer<QWebSocket> peer;
    QList<QJsonObject> packets;
    int connections = 0;
    ChassisFixture() : QWebSocketServer("manual-test", QWebSocketServer::NonSecureMode) {
        listen(QHostAddress::LocalHost);
        connect(this, &QWebSocketServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto *socket = nextPendingConnection();
                peer = socket;
                ++connections;
                connect(socket, &QWebSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QWebSocket::textMessageReceived, this, [this](const QString &text) {
                    packets.append(QJsonDocument::fromJson(text.toUtf8()).object());
                });
            }
        });
    }
    QUrl url() const { return QUrl(QString("ws://127.0.0.1:%1").arg(serverPort())); }
};
using Queue = ChassisVelocityQueue;
}

class ChassisQueueTest : public QObject {
    Q_OBJECT
private slots:
    void congestionKeepsOnlyLatestAndDoesNotReplayTheReplacedSample() {
        Queue queue;
        QCOMPARE(queue.submit(0, 0, {.1, 0}).kind, Queue::Kind::Send);
        QCOMPARE(queue.submit(40, 120, {.2, .1}).kind, Queue::Kind::None);
        QCOMPARE(queue.submit(80, 120, {.3, .2}).kind, Queue::Kind::None);
        const auto drained = queue.advance(90, 0);
        QCOMPARE(drained.kind, Queue::Kind::Send);
        QCOMPARE(drained.velocity.linear, .3); QCOMPARE(drained.velocity.angular, .2);
        QCOMPARE(queue.advance(100, 0).kind, Queue::Kind::None);
        QVERIFY(!queue.needsService());
        QCOMPARE(queue.advance(200, 0).kind, Queue::Kind::None);
    }
    void freshHeartbeatsCannotExtendTheOldestLocalBacklog() {
        Queue queue;
        queue.submit(0, 0, {.1, 0});
        for (qint64 now : {40, 80, 120, 149})
            QCOMPARE(queue.submit(now, 120, {.2, 0}).kind, Queue::Kind::None);
        const auto expired = queue.advance(Queue::MaxAgeMs, 120);
        QCOMPARE(expired.kind, Queue::Kind::Disconnect);
        QCOMPARE(expired.failure, Queue::Failure::WriteExpired);
        QVERIFY(!queue.needsService());
        QCOMPARE(queue.advance(160, 0).kind, Queue::Kind::None);
    }
    void stalePendingSampleIsNotSentAfterTheSocketDrains() {
        Queue queue;
        queue.submit(0, 0, {.1, 0});
        queue.submit(10, 100, {.2, 0});
        const auto expired = queue.advance(160, 0);
        QCOMPARE(expired.kind, Queue::Kind::Disconnect);
        QCOMPARE(expired.failure, Queue::Failure::SampleExpired);
        QCOMPARE(queue.advance(170, 0).kind, Queue::Kind::None);
    }
    void stopUnderBackpressureDisconnectsWithoutQueuingBehindMotion() {
        Queue queue;
        queue.submit(0, 0, {.1, 0}); queue.submit(40, 100, {.2, 0});
        const auto stopped = queue.submit(41, 100, {0, 0});
        QCOMPARE(stopped.kind, Queue::Kind::Disconnect);
        QCOMPARE(stopped.failure, Queue::Failure::StopBlocked);
        QVERIFY(!queue.needsService());
        QCOMPARE(queue.advance(50, 0).kind, Queue::Kind::None);
    }
    void stopReplacesTheUnsentMotionAsSoonAsLocalBufferIsEmpty() {
        Queue queue;
        queue.submit(0, 0, {.1, 0}); queue.submit(40, 100, {.2, 0});
        const auto stopped = queue.submit(41, 0, {0, 0});
        QCOMPARE(stopped.kind, Queue::Kind::Send);
        QCOMPARE(stopped.velocity.linear, 0.0); QCOMPARE(stopped.velocity.angular, 0.0);
        QCOMPARE(queue.advance(50, 0).kind, Queue::Kind::None);
    }
    void disconnectClearsPendingAndRequiresANewExplicitSample() {
        Queue queue;
        queue.submit(0, 0, {.1, 0}); queue.submit(40, 100, {.2, 0});
        queue.clear();
        QCOMPARE(queue.advance(1000, 0).kind, Queue::Kind::None);
        QCOMPARE(queue.submit(1040, 0, {.3, 0}).kind, Queue::Kind::Send);
    }
    void byteLimitAndMonotonicClockRemainFailClosed() {
        Queue queue;
        QCOMPARE(queue.submit(100, 0, {.1, 0}).kind, Queue::Kind::Send);
        QCOMPARE(queue.advance(99, 0).failure, Queue::Failure::ClockRewound);
        QCOMPARE(queue.submit(200, Queue::MaxBufferedBytes + 1, {.1, 0}).failure, Queue::Failure::TooManyBytes);
        QVERIFY(!queue.needsService());
    }
    void loopbackPreservesProtocolAndNormalHeartbeatAndStop() {
        ChassisFixture server;
        ChassisClient client; client.setAutoReconnect(false); client.setUrl(server.url()); client.connectToHost();
        QTRY_VERIFY(client.isConnected());
        for (int index = 0; index < 5; ++index) {
            client.sendVelocityCommand(.15, .2);
            QTest::qWait(40); // Existing 25 Hz manual heartbeat remains supported.
        }
        QTRY_COMPARE(server.packets.size(), 5);
        for (const auto &packet : server.packets) {
            QCOMPARE(packet["packet"].toObject(), QJsonObject({{"cmd", "region"}, {"region", "cmd_vel"}, {"index", 1}}));
            const auto msg = packet["msg"].toObject();
            QCOMPARE(msg["xvel"].toDouble(), .15); QCOMPARE(msg["thetavel"].toDouble(), .2);
            QVERIFY(msg["isRemote"].toBool());
        }
        client.sendVelocityCommand(0, 0);
        QTRY_COMPARE(server.packets.size(), 6);
        QCOMPARE(server.packets.last()["msg"].toObject()["xvel"].toDouble(), 0.0);
        QTest::qWait(180);
        QCOMPARE(server.packets.size(), 6); // Timer service never invents another heartbeat.
        client.sendVelocityCommand(std::numeric_limits<double>::quiet_NaN(), 0);
        QTest::qWait(40); QCOMPARE(server.packets.size(), 6);
        client.disconnectFromHost();
    }
    void reconnectNeverReplaysAndDisconnectNotifiesTheManualOwner() {
        ChassisFixture server;
        ChassisClient client; client.setAutoReconnect(true); client.setReconnectIntervalMs(200); client.setUrl(server.url());
        QSignalSpy disconnected(&client, &ChassisClient::disconnected);
        client.connectToHost(); QTRY_VERIFY(client.isConnected());
        client.sendVelocityCommand(.1, 0); QTRY_COMPARE(server.packets.size(), 1);
        server.peer->abort();
        QTRY_COMPARE(disconnected.size(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(server.connections, 2, 1200);
        QTRY_VERIFY(client.isConnected()); QTest::qWait(180);
        QCOMPARE(server.packets.size(), 1);
        client.sendVelocityCommand(.2, 0); QTRY_COMPARE(server.packets.size(), 2);
        client.disconnectFromHost(); QTRY_COMPARE(disconnected.size(), 2);
        client.sendVelocityCommand(.3, 0);
        QTest::qWait(250); QCOMPARE(server.connections, 2); QCOMPARE(server.packets.size(), 2);
    }
    void realDeadlineTimerClearsHeldInputAndReconnectNeverResumesIt() {
        ChassisFixture server;
        ChassisClient client;
        MotionCommandArbiter manual;
        manual.setManualCommandConfig({.1, .2, true, true});
        client.setUrl(server.url()); client.setReconnectIntervalMs(200);
        connect(&client, &ChassisClient::connected, &manual, [&] { manual.setChassisConnected(true); });
        connect(&client, &ChassisClient::disconnected, &manual, [&] { manual.setChassisConnected(false); });
        connect(&manual, &MotionCommandArbiter::velocityCommand, &client, &ChassisClient::sendVelocityCommand);
        QSignalSpy disconnected(&client, &ChassisClient::disconnected);
        QSignalSpy errors(&client, &ChassisClient::errorOccurred);
        QSignalSpy commands(&manual, &MotionCommandArbiter::velocityCommand);
        QSignalSpy stops(&manual, &MotionCommandArbiter::safetyStopRequested);
        client.connectToHost(); QTRY_VERIFY(client.isConnected());
        manual.setManualInputActive(MotionCommandArbiter::ManualInput::Forward, true, false);
        QVERIFY(!commands.isEmpty());
        QTRY_VERIFY(!server.packets.isEmpty());
        // Deterministically inject an old unsent sample; all transport/timer,
        // abort, disconnect notification and manual ownership below are real.
        const auto now = client.m_velocityClock.elapsed();
        client.clearVelocityQueue();
        client.m_velocityQueue.submit(now - 200, 0, {.1, 0});
        client.m_velocityQueue.submit(now - 190, 100, {.2, 0});
        client.m_velocityTimer->start();
        QTRY_COMPARE_WITH_TIMEOUT(disconnected.size(), 1, 250);
        QCOMPARE(errors.size(), 1);
        QCOMPARE(stops.size(), 1);
        QCOMPARE(stops.first().first().toString(), QString("chassis_disconnected"));
        QVERIFY(!client.m_velocityQueue.needsService());
        QVERIFY(!client.m_velocityTimer->isActive());
        const auto commandsAfterDisconnect = commands.size();
        QTest::qWait(20); // Drain an already delivered initial command at the peer.
        const auto packetsAfterDisconnect = server.packets.size();
        QTRY_COMPARE_WITH_TIMEOUT(server.connections, 2, 1200);
        QTRY_VERIFY(client.isConnected()); QTest::qWait(160);
        QCOMPARE(commands.size(), commandsAfterDisconnect);
        QCOMPARE(server.packets.size(), packetsAfterDisconnect);
        manual.setManualInputActive(MotionCommandArbiter::ManualInput::Forward, true, false);
        QVERIFY(commands.size() > commandsAfterDisconnect); // A fresh press is required.
        QTRY_VERIFY(server.packets.size() > packetsAfterDisconnect);
        manual.stopAll("test_stop", true);
        client.disconnectFromHost();
    }
    void deadlineDisconnectObserverCanDestroyClientSynchronously() {
        ChassisFixture server;
        QPointer<ChassisClient> client = new ChassisClient;
        client->setUrl(server.url()); client->connectToHost();
        QTRY_VERIFY(client->isConnected());
        int notifications = 0;
        connect(client, &ChassisClient::disconnected, this, [&] {
            ++notifications;
            delete client.data();
        });
        const auto now = client->m_velocityClock.elapsed();
        client->m_velocityQueue.submit(now - 200, 0, {.1, 0});
        client->m_velocityQueue.submit(now - 190, 100, {.2, 0});
        client->m_velocityTimer->start();
        QTRY_VERIFY(!client);
        QCOMPARE(notifications, 1);
        QTest::qWait(250);
        QCOMPARE(server.connections, 1); QCOMPARE(server.packets.size(), 0);
    }
    void destructionCancelsTimerAndReconnectWithoutLateDelivery_data() {
        QTest::addColumn<bool>("disconnectFirst");
        QTest::newRow("connected") << false;
        QTest::newRow("reconnect-scheduled") << true;
    }
    void destructionCancelsTimerAndReconnectWithoutLateDelivery() {
        QFETCH(bool, disconnectFirst);
        ChassisFixture server;
        auto *client = new ChassisClient;
        client->setUrl(server.url()); client->setReconnectIntervalMs(200); client->connectToHost();
        QTRY_VERIFY(client->isConnected());
        client->sendVelocityCommand(.1, 0); QTRY_COMPARE(server.packets.size(), 1);
        if (disconnectFirst) { server.peer->abort(); QTRY_VERIFY(!client->isConnected()); }
        delete client;
        QTest::qWait(300);
        QCOMPARE(server.connections, 1); QCOMPARE(server.packets.size(), 1);
    }
};

QTEST_GUILESS_MAIN(ChassisQueueTest)
#include "test_chassis_queue.moc"
