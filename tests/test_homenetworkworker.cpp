#include "homenetworkworker.h"
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>
#include <memory>

class StatusFixture : public QTcpServer {
public:
    int requests = 0;
    QPointer<QTcpSocket> pending;
    QElapsedTimer clock;
    QList<qint64> receivedAt;
    StatusFixture() {
        listen(QHostAddress::LocalHost);
        clock.start();
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto *socket = nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                auto bytes = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, bytes] {
                    bytes->append(socket->readAll());
                    if (socket->property("handled").toBool() || !bytes->contains("\r\n\r\n")) return;
                    socket->setProperty("handled", true);
                    ++requests;
                    receivedAt.append(clock.elapsed());
                    pending = socket;
                });
            }
        });
    }
    QUrl url() const { return QUrl(QString("http://127.0.0.1:%1/status").arg(serverPort())); }
    void respond(int status = 200) {
        QVERIFY(pending);
        auto *socket = pending.data(); pending.clear();
        const QByteArray body = QJsonDocument(QJsonObject{{"online", true}}).toJson(QJsonDocument::Compact);
        socket->write("HTTP/1.1 " + QByteArray::number(status) + " Result\r\nContent-Type: application/json\r\nContent-Length: " +
            QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
    }
};

class HomeNetworkWorkerTest : public QObject {
    Q_OBJECT
private slots:
    void slowResponsesDoNotQueueCatchupRequests() {
        StatusFixture server;
        HomeNetworkWorker worker;
        worker.configure(server.url(), QJsonArray{QJsonObject{{"read", "status"}}}, 120, {}, 1000, 1000);
        QSignalSpy received(&worker, &HomeNetworkWorker::statusReceived);
        worker.start();
        QTRY_COMPARE(server.requests, 1);
        QTest::qWait(180);
        for (int index = 0; index < 10; ++index) worker.triggerFetch();
        QCOMPARE(server.requests, 1);
        const auto completedAt = server.clock.elapsed();
        server.respond();
        QTRY_COMPARE(received.count(), 1);
        QTest::qWait(50);
        QCOMPARE(server.requests, 1);
        QTRY_COMPARE(server.requests, 2);
        QVERIFY(server.receivedAt.at(1) - completedAt >= 110);
        worker.stop();
    }
    void failuresRespectBackoffAfterSlowResponses() {
        StatusFixture server;
        HomeNetworkWorker worker;
        worker.configure(server.url(), QJsonArray{QJsonObject{{"read", "status"}}}, 100, {}, 1000, 1000);
        QSignalSpy failed(&worker, &HomeNetworkWorker::requestFailed);
        worker.start();
        QTRY_COMPARE(server.requests, 1);
        QTest::qWait(150);
        const auto completedAt = server.clock.elapsed();
        server.respond(503);
        QTRY_COMPARE(failed.count(), 1);
        QTest::qWait(90);
        QCOMPARE(server.requests, 1);
        QTRY_COMPARE(server.requests, 2);
        QVERIFY(server.receivedAt.at(1) - completedAt >= 190);
        worker.stop();
    }
    void stopCancelsOutstandingReadAndRestartHasNoQueuedDebt() {
        StatusFixture server;
        HomeNetworkWorker worker;
        worker.configure(server.url(), QJsonArray{QJsonObject{{"read", "status"}}}, 100, {}, 1000, 1000);
        QSignalSpy received(&worker, &HomeNetworkWorker::statusReceived);
        worker.start();
        QTRY_COMPARE(server.requests, 1);
        worker.triggerFetch();
        worker.stop();
        QTest::qWait(250);
        QCOMPARE(server.requests, 1);
        QCOMPARE(received.count(), 0);
        worker.start();
        QTRY_COMPARE(server.requests, 2);
        server.respond();
        QTRY_COMPARE(received.count(), 1);
        worker.stop();
        QTest::qWait(150);
        QCOMPARE(server.requests, 2);
    }
    void callbacksCanRestartWithoutOldPollTimer_data() {
        QTest::addColumn<bool>("failure");
        QTest::newRow("status") << false;
        QTest::newRow("failure") << true;
    }
    void callbacksCanRestartWithoutOldPollTimer() {
        QFETCH(bool, failure);
        StatusFixture server;
        HomeNetworkWorker worker;
        worker.configure(server.url(), QJsonArray{QJsonObject{{"read", "status"}}}, 100, {}, 1000, 1000);
        bool restarted = false;
        const auto restart = [&] {
            if (restarted) return;
            restarted = true;
            worker.stop();
            worker.start();
        };
        connect(&worker, &HomeNetworkWorker::statusReceived, &worker, restart);
        connect(&worker, &HomeNetworkWorker::requestFailed, &worker, restart);
        worker.start();
        QTRY_COMPARE(server.requests, 1);
        server.respond(failure ? 503 : 200);
        QTRY_COMPARE(server.requests, 2);
        QTest::qWait(160);
        QCOMPARE(server.requests, 2);
        const auto completedAt = server.clock.elapsed();
        server.respond();
        QTest::qWait(40);
        QCOMPARE(server.requests, 2);
        QTRY_COMPARE(server.requests, 3);
        QVERIFY(server.receivedAt.at(2) - completedAt >= 90);
        worker.stop();
    }
    void configureReplacesOutstandingRead() {
        StatusFixture previous, replacement;
        HomeNetworkWorker worker;
        const QJsonArray reads{QJsonObject{{"read", "status"}}};
        worker.configure(previous.url(), reads, 100, {}, 1000, 1000);
        QSignalSpy received(&worker, &HomeNetworkWorker::statusReceived);
        worker.start();
        QTRY_COMPARE(previous.requests, 1);
        worker.configure(replacement.url(), reads, 100, {}, 1000, 1000);
        QTRY_COMPARE(replacement.requests, 1);
        replacement.respond();
        QTRY_COMPARE(received.count(), 1);
        worker.stop();
        QTest::qWait(150);
        QCOMPARE(previous.requests, 1);
        QCOMPARE(replacement.requests, 1);
        QCOMPARE(received.count(), 1);
    }
};
QTEST_GUILESS_MAIN(HomeNetworkWorkerTest)
#include "test_homenetworkworker.moc"
