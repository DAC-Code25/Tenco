#include "poseclient.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QtTest>

namespace {
class PoseStreamFixture : public QTcpServer {
  public:
    QPointer<QTcpSocket> current;
    int requests = 0;
    QElapsedTimer sourceClock;
    std::function<void()> connected;

    PoseStreamFixture() {
        sourceClock.start();
        listen(QHostAddress::LocalHost);
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto *socket = nextPendingConnection();
                auto request = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, request] {
                    request->append(socket->readAll());
                    if (socket->property("handled").toBool() || !request->contains("\r\n\r\n"))
                        return;
                    socket->setProperty("handled", true);
                    ++requests;
                    current = socket;
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/x-ndjson\r\n"
                                  "Transfer-Encoding: chunked\r\nCache-Control: no-store\r\n\r\n");
                    if (connected)
                        connected();
                });
            }
        });
    }
    QUrl url() const { return QUrl(QString("http://127.0.0.1:%1/api/v1").arg(serverPort())); }
    double now() const { return sourceClock.elapsed() / 1000.0; }
    QJsonObject pose(quint64 seq, double sent = -1, const QString &boot = "pose-a") const {
        return {{"apiVersion", 1}, {"seq", QString::number(seq)}, {"bootId", boot},
                {"frameId", "map"}, {"baseFrameId", "base_link"},
                {"originRevision", "origin-a"}, {"calibrationId", "cal-a"},
                {"clockDomain", "system"}, {"localizationMode", "degraded"},
                {"freshnessBasis", "http_receipt"}, {"sendMonotonicSeconds", sent < 0 ? now() : sent},
                {"sendTime", 10000.0}, {"stateAgeMs", 0},
                {"positionObservationAgeMs", 0}, {"headingObservationAgeMs", 0},
                {"pose", QJsonObject{{"x", double(seq)}, {"y", 0}, {"yaw", 0}}},
                {"estimatedTwist", QJsonObject{{"v", 0}, {"omega", 0}}},
                {"validForControl", true}};
    }
    static QByteArray line(const QJsonObject &pose) {
        return QJsonDocument(pose).toJson(QJsonDocument::Compact) + '\n';
    }
    void send(const QByteArray &bytes) {
        if (current)
            current->write(QByteArray::number(bytes.size(), 16) + "\r\n" + bytes + "\r\n");
    }
    void send(const QJsonObject &value) { send(line(value)); }
};
}

class PoseWirelessTest : public QObject {
    Q_OBJECT
  private slots:
    void monitoringTracksDeliveryIndependentlyOfControlValidity() {
        PoseStreamFixture server;
        quint64 seq = 1;
        server.connected = [&] { server.send(server.pose(seq)); };
        PoseClient client;
        client.configure(server.url(), {}, 5000);
        QSignalSpy controls(&client, &PoseClient::availabilityChanged);
        QSignalSpy monitors(&client, &PoseClient::monitoringAvailabilityChanged);
        ControlPoseSnapshot displayed;
        bool monitorCurrent = false;
        // Match the homepage's signal order: read the current snapshot before
        // applying monitoring availability, including stop() clearing its cache.
        connect(&client, &PoseClient::monitoringAvailabilityChanged, this,
            [&](bool current, const QString &) { displayed = client.snapshot(); monitorCurrent = current; });
        client.start();
        QTRY_VERIFY(client.fresh());
        QTRY_VERIFY(monitorCurrent);
        QVERIFY(!displayed.monitorDelayed);

        QString reason = "initializing";
        bool validForControl = false;
        QTimer publisher;
        publisher.setInterval(20);
        connect(&publisher, &QTimer::timeout, this, [&] {
            auto value = server.pose(++seq);
            value["validForControl"] = validForControl;
            value["reasonCodes"] = QJsonArray{reason};
            // Fusion's source state may be old while its diagnostic frames
            // continue arriving promptly. That is not a monitoring outage.
            value["stateAgeMs"] = validForControl ? 0 : 10000;
            server.send(value);
        });
        monitors.clear(); controls.clear();
        publisher.start();
        QTRY_VERIFY(!client.fresh());
        QTRY_VERIFY(!controls.isEmpty());
        QVERIFY(!controls.last().at(0).toBool());
        QTest::qWait(450);
        QVERIFY(!client.snapshot().validForControl);
        QVERIFY(client.snapshot().reasons.contains("initializing"));
        QVERIFY(!client.snapshot().monitorDelayed);
        QVERIFY(monitorCurrent);
        QCOMPARE(monitors.size(), 0);
        reason = "nonfinite_estimate";
        QTRY_VERIFY(client.snapshot().reasons.contains(reason));
        QTest::qWait(400);
        QVERIFY(!client.fresh());
        QVERIFY(!client.snapshot().monitorDelayed);
        QCOMPARE(monitors.size(), 0);

        publisher.stop();
        QTRY_VERIFY(!monitorCurrent);
        QVERIFY(!monitors.isEmpty());
        QVERIFY(!monitors.last().at(0).toBool());
        QVERIFY(displayed.monitorDelayed);
        QVERIFY(!displayed.validForControl);
        publisher.start();
        QTRY_VERIFY(monitorCurrent);
        QVERIFY(!displayed.monitorDelayed);
        QVERIFY(!displayed.validForControl);
        validForControl = true; reason.clear();
        QTRY_VERIFY(client.fresh());
        QTRY_VERIFY(controls.last().at(0).toBool());
        QVERIFY(!client.snapshot().monitorDelayed);
        publisher.stop();
        const auto monitoringCount = monitors.size();
        client.stop();
        QCOMPARE(monitors.size(), monitoringCount + 1);
        QVERIFY(!monitorCurrent);
        QVERIFY(displayed.bootId.isEmpty());
        QVERIFY(!displayed.monitorDelayed);
        QVERIFY(!displayed.validForControl);
        QCOMPARE(server.requests, 1);
    }

    void mergedBurstPublishesLatestOnceAndPreservesCoordinateChanges() {
        PoseStreamFixture server;
        server.connected = [&] { server.send(server.pose(1)); };
        PoseClient client;
        client.configure(server.url(), {}, 5000);
        QSignalSpy poses(&client, &PoseClient::poseChanged), frames(&client, &PoseClient::frameChanged);
        client.start();
        QTRY_VERIFY(client.fresh());
        poses.clear();
        QByteArray burst;
        for (quint64 seq = 2; seq <= 41; ++seq) {
            auto value = server.pose(seq);
            if (seq == 20)
                value["originRevision"] = "origin-b";
            burst += PoseStreamFixture::line(value);
        }
        server.send(burst);
        QTRY_COMPARE(client.snapshot().seq, quint64(41));
        QCOMPARE(poses.size(), 1);
        QCOMPARE(frames.size(), 2);
        QCOMPARE(client.snapshot().position.x(), 41.0);
        QVERIFY(client.fresh());
        client.stop();
    }

    void invalidIntermediateFrameCannotBeHiddenByLaterValidFrame() {
        PoseStreamFixture server;
        server.connected = [&] { server.send(server.pose(1)); };
        PoseClient client;
        client.configure(server.url(), {}, 5000);
        QSignalSpy poses(&client, &PoseClient::poseChanged), errors(&client, &PoseClient::errorOccurred);
        QSignalSpy monitors(&client, &PoseClient::monitoringAvailabilityChanged);
        client.start();
        QTRY_VERIFY(client.fresh());
        poses.clear();
        auto invalid = server.pose(3);
        invalid["pose"] = QJsonObject{{"x", 3}, {"y", 0}};
        server.send(PoseStreamFixture::line(server.pose(2)) + PoseStreamFixture::line(invalid) +
                    PoseStreamFixture::line(server.pose(4)));
        QTRY_COMPARE(errors.size(), 1);
        QCOMPARE(client.snapshot().seq, quint64(2));
        QCOMPARE(poses.size(), 0);
        QVERIFY(!client.fresh());
        QVERIFY(!monitors.isEmpty());
        QVERIFY(!monitors.last().at(0).toBool());
        QVERIFY(client.snapshot().monitorDelayed);
        client.stop();
    }

    void wirelessGapAndBurstRetainAgeThenRecoverWithoutReconnect() {
        PoseStreamFixture server;
        server.connected = [&] { server.send(server.pose(1, 0)); };
        PoseClient client;
        client.configure(server.url(), {}, 5000);
        QSignalSpy poses(&client, &PoseClient::poseChanged), errors(&client, &PoseClient::errorOccurred);
        client.start();
        QTRY_VERIFY(client.fresh());
        poses.clear();
        QTest::qWait(850);
        QVERIFY(!client.fresh());
        QVERIFY(client.snapshot().monitorDelayed);
        QByteArray burst;
        for (quint64 seq = 2; seq <= 8; ++seq)
            burst += PoseStreamFixture::line(server.pose(seq, double(seq - 1) * .02));
        server.send(burst);
        QTRY_COMPARE(client.snapshot().seq, quint64(8));
        QCOMPARE(poses.size(), 1);
        QVERIFY(client.snapshot().stateAgeMs > 600);
        QVERIFY(!client.snapshot().validForControl);
        QVERIFY(client.snapshot().monitorDelayed);
        server.send(server.pose(9));
        QTRY_VERIFY(client.fresh());
        QVERIFY(!client.snapshot().monitorDelayed);
        QCOMPARE(client.snapshot().seq, quint64(9));
        QCOMPARE(server.requests, 1);
        QCOMPARE(errors.size(), 0);
        client.stop();
    }

    void reconnectDoesNotRebaseUnseenQueuedFrameOrRefreshReplay() {
        PoseStreamFixture server;
        server.connected = [&] { server.send(server.pose(1, 0)); };
        PoseClient client;
        client.configure(server.url(), {}, 5000);
        QSignalSpy poses(&client, &PoseClient::poseChanged);
        QSignalSpy monitors(&client, &PoseClient::monitoringAvailabilityChanged);
        QSignalSpy streamErrors(&client, &PoseClient::streamErrorOccurred);
        client.start();
        QTRY_VERIFY(client.fresh());
        QTRY_VERIFY(!monitors.isEmpty() && monitors.last().at(0).toBool());
        QTest::qWait(450);
        server.current->abort();
        QTRY_COMPARE(streamErrors.size(), 1);
        QCOMPARE(streamErrors.first().first().toString(), QString("pose_stream_disconnected"));
        QTRY_COMPARE_WITH_TIMEOUT(server.requests, 2, 1500);
        QTest::qWait(60);
        QCOMPARE(poses.size(), 1);
        QVERIFY(!client.fresh());
        QVERIFY(!monitors.last().at(0).toBool());
        QVERIFY(client.snapshot().monitorDelayed);
        const auto outageMonitoringCount = monitors.size();
        server.send(server.pose(2, .02));
        QTRY_COMPARE(client.snapshot().seq, quint64(2));
        QVERIFY2(client.snapshot().stateAgeMs > 600, "Reconnect must retain the same producer's age baseline");
        QVERIFY(!client.fresh());
        QTest::qWait(60); // Give the availability timer a chance to reject the queued frame.
        QCOMPARE(monitors.size(), outageMonitoringCount);
        QVERIFY(!monitors.last().at(0).toBool());
        QVERIFY(client.snapshot().monitorDelayed);

        // Current diagnostic frames restore monitoring even while fusion is
        // initializing. They must not claim a usable control pose.
        auto initializing = server.pose(3);
        initializing["validForControl"] = false;
        initializing["stateAgeMs"] = 10000;
        initializing["reasonCodes"] = QJsonArray{"initializing"};
        server.send(initializing);
        QTRY_COMPARE(client.snapshot().seq, quint64(3));
        QTRY_VERIFY(monitors.last().at(0).toBool());
        QCOMPARE(monitors.size(), outageMonitoringCount + 1);
        QVERIFY(monitors.last().at(1).toString().isEmpty());
        QVERIFY(!client.snapshot().monitorDelayed);
        QVERIFY(!client.fresh());
        QVERIFY(client.snapshot().reasons.contains("initializing"));

        server.send(server.pose(4));
        QTRY_VERIFY(client.fresh());
        QCOMPARE(client.snapshot().seq, quint64(4));
        QCOMPARE(monitors.size(), outageMonitoringCount + 1);
        QCOMPARE(streamErrors.size(), 1);
        client.stop();
    }

    void producerBootChangeStartsNewTimeDomainAndRemainsVisible() {
        PoseStreamFixture server;
        server.connected = [&] { server.send(server.pose(1, 100)); };
        PoseClient client;
        client.configure(server.url(), {}, 5000);
        QSignalSpy frames(&client, &PoseClient::frameChanged), errors(&client, &PoseClient::errorOccurred);
        client.start();
        QTRY_VERIFY(client.fresh());
        server.send(server.pose(1, 0, "pose-b"));
        QTRY_COMPARE(client.snapshot().bootId, QString("pose-b"));
        QCOMPARE(frames.size(), 1);
        QCOMPARE(errors.size(), 0);
        QVERIFY(client.fresh());
        client.stop();
    }

    void stopCancelsPendingDeliveryAndReconnect() {
        PoseStreamFixture server;
        server.connected = [&] { server.send(server.pose(1)); };
        PoseClient client;
        client.configure(server.url(), {}, 5000);
        QSignalSpy poses(&client, &PoseClient::poseChanged);
        client.start();
        QTRY_VERIFY(client.fresh());
        server.current->abort();
        client.stop();
        const auto count = poses.size();
        QTest::qWait(350);
        QCOMPARE(server.requests, 1);
        QCOMPARE(poses.size(), count);
        QVERIFY(!client.fresh());
    }
};

QTEST_GUILESS_MAIN(PoseWirelessTest)
#include "test_pose_wireless.moc"
