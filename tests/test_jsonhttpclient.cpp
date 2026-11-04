#include "jsonhttpclient.h"
#include <QJsonDocument>
#include <QPointer>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QtTest>

class TimingFixture : public QTcpServer {
  public:
    int connections = 0;
    std::function<void(QTcpSocket *, QByteArray, QByteArray)> handler;
    TimingFixture() {
        listen(QHostAddress::LocalHost);
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto *socket = nextPendingConnection();
                ++connections;
                auto bytes = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, bytes] {
                    bytes->append(socket->readAll());
                    for (;;) {
                        const int end = bytes->indexOf("\r\n\r\n");
                        if (end < 0) return;
                        const auto headers = bytes->left(end);
                        for (const auto &line : headers.split('\n'))
                            if (line.toLower().startsWith("x-request-id:"))
                                socket->setProperty("requestId", line.mid(13).trimmed());
                        int length = 0;
                        for (const auto &line : headers.split('\n'))
                            if (line.toLower().startsWith("content-length:"))
                                length = line.mid(15).trimmed().toInt();
                        if (bytes->size() < end + 4 + length) return;
                        const auto body = bytes->mid(end + 4, length);
                        bytes->remove(0, end + 4 + length);
                        handler(socket, headers.split(' ').value(1), body);
                    }
                });
            }
        });
    }
    QUrl url() const { return QUrl(QString("http://127.0.0.1:%1/api/v1").arg(serverPort())); }
    static void reply(QTcpSocket *socket, const QByteArray &body = "{\"ok\":true}", const QByteArray &extraHeaders = {}) {
        socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                      QByteArray::number(body.size()) + "\r\nConnection: keep-alive\r\n" + extraHeaders + "\r\n" + body);
    }
};

class JsonHttpClientTest : public QObject {
    Q_OBJECT
  private slots:
    void correlationAssociatesOnlyBoundedServerDurationsWithoutSecrets() {
        TimingFixture server;
        QList<QByteArray> ids;
        server.handler = [&](QTcpSocket *socket, QByteArray, QByteArray) {
            const auto id = socket->property("requestId").toByteArray();
            ids.append(id);
            QVERIFY(QRegularExpression("^[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}$").match(QString::fromLatin1(id)).hasMatch());
            TimingFixture::reply(socket, "{\"ok\":true}", "X-Request-ID: " + id +
                "\r\nServer-Timing: body;dur=0.25, app;dur=31.5;desc=secret-body, queue;dur=1, serialize;dur=2.125, dispatch;dur=0, secret-token;dur=99\r\n");
        };
        JsonHttpClient client;
        client.configure(server.url(), "secret-bearer", 2000);
        QSignalSpy measurements(&client, &JsonHttpClient::requestMeasured);
        QList<JsonHttpResult> results;
        for (int index = 0; index < 2; ++index) {
            QVERIFY(client.request("status", "GET", "/secret-path", {}, QUrlQuery("secret-query=value"), "secret-operator",
                [&](const JsonHttpResult &result) { results.append(result); }));
            QTRY_COMPARE(results.size(), index + 1);
            QCOMPARE(results.last().requestId.toLatin1(), ids.last());
            QVERIFY(results.last().serverRequestIdMatched);
            QCOMPARE(results.last().serverTimingMs.size(), 5);
            QCOMPARE(results.last().serverTimingMs.value("app").toDouble(), 31.5);
            const auto recorded = measurements.last().at(1).toJsonObject();
            QCOMPARE(recorded.value("requestId").toString(), results.last().requestId);
            QCOMPARE(recorded.value("serverTimingMs").toObject(), results.last().serverTimingMs);
            QVERIFY(!QJsonDocument(recorded).toJson().contains("secret"));
        }
        QVERIFY(ids.at(0) != ids.at(1));
    }
    void invalidOrUncorrelatedServerTimingIsIgnored_data() {
        QTest::addColumn<QByteArray>("timing");
        QTest::addColumn<bool>("matchingId");
        QTest::newRow("mismatch") << QByteArray("app;dur=10") << false;
        QTest::newRow("invalid-values") << QByteArray("body;dur=-1, app;dur=nan, queue;dur=inf, serialize;dur=3600001, dispatch;dur=1;dur=2") << true;
        QTest::newRow("oversize") << QByteArray(2049, 'x') << true;
    }
    void invalidOrUncorrelatedServerTimingIsIgnored() {
        QFETCH(QByteArray, timing);
        QFETCH(bool, matchingId);
        TimingFixture server;
        server.handler = [&](QTcpSocket *socket, QByteArray, QByteArray) {
            const auto id = matchingId ? socket->property("requestId").toByteArray() : QByteArray("untrusted-secret-id");
            TimingFixture::reply(socket, "{\"ok\":true}", "X-Request-ID: " + id + "\r\nServer-Timing: " + timing + "\r\n");
        };
        JsonHttpClient client;
        client.configure(server.url(), {}, 2000);
        QSignalSpy measurements(&client, &JsonHttpClient::requestMeasured);
        bool complete = false;
        QVERIFY(client.request("status", "GET", "/health", {}, {}, {}, [&](const JsonHttpResult &result) {
            QVERIFY(result.ok());
            QCOMPARE(result.serverRequestIdMatched, matchingId);
            QVERIFY(result.serverTimingMs.isEmpty());
            complete = true;
        }));
        QTRY_VERIFY(complete);
        QVERIFY(!QJsonDocument(measurements.first().at(1).toJsonObject()).toJson().contains("secret"));
    }
    void delayedResponseReportsMilestonesWithoutSecrets() {
        TimingFixture server;
        server.handler = [](QTcpSocket *socket, QByteArray, QByteArray) {
            QTimer::singleShot(180, socket, [socket] { TimingFixture::reply(socket); });
        };
        JsonHttpClient client;
        client.configure(server.url(), "bearer-test-secret", 2000);
        QSignalSpy measurements(&client, &JsonHttpClient::requestMeasured);
        JsonHttpResult response;
        int callbacks = 0;
        QVERIFY(client.request("status", "GET", "/secret-path", {}, {}, "operator-test-secret",
                               [&](const JsonHttpResult &r) { response = r; ++callbacks; }));
        QTRY_COMPARE(callbacks, 1);
        QVERIFY(response.ok());
        QVERIFY(response.requestSentMs >= 0);
        QVERIFY(response.firstResponseMs >= 150);
        QVERIFY(response.firstResponseMs >= response.requestSentMs);
        QVERIFY(response.roundTripMs >= response.firstResponseMs);
        QCOMPARE(response.sendAttempts, 1);
        QCOMPARE(measurements.count(), 1);
        const auto object = measurements.first()[1].toJsonObject();
        const auto serialized = QJsonDocument(object).toJson();
        QVERIFY(!serialized.contains("secret"));
        QVERIFY(!object.contains("url") && !object.contains("body") && !object.contains("error"));
    }

    void saturatedMonitoringCannotQueueOperationsOrTheirResult() {
        TimingFixture server;
        int monitors = 0, operations = 0;
        server.handler = [&](QTcpSocket *socket, QByteArray path, QByteArray body) {
            if (path.contains("monitor")) { ++monitors; return; }
            if (path.endsWith("/write")) QCOMPARE(body, QByteArray("{\"requestId\":\"one\"}"));
            ++operations;
            TimingFixture::reply(socket);
        };
        JsonHttpClient client;
        client.configure(server.url(), {}, 3000);
        int monitorCallbacks = 0;
        for (int i = 0; i < 6; ++i)
            QVERIFY(client.request(QString("monitor-%1").arg(i), "GET", "/monitor", {}, {}, {},
                                   [&](const JsonHttpResult &) { ++monitorCallbacks; }));
        QTRY_COMPARE(monitors, 6);
        QElapsedTimer age;
        age.start();
        int finished = 0;
        QVERIFY(client.request("write", "POST", "/write", {{"requestId", "one"}}, {}, {},
                               [&](const JsonHttpResult &r) { QVERIFY(r.ok()); ++finished; }));
        QTRY_COMPARE_WITH_TIMEOUT(finished, 1, 1000);
        QVERIFY(age.elapsed() < 1000);
        QVERIFY(client.request("result", "GET", "/commands/one", {}, {}, {},
                               [&](const JsonHttpResult &r) { QVERIFY(r.ok()); ++finished; }));
        QTRY_COMPARE_WITH_TIMEOUT(finished, 2, 1000);
        QCOMPARE(operations, 2);
        QCOMPARE(monitorCallbacks, 0);
        client.cancelAll();
        QTest::qWait(50);
        QCOMPARE(monitorCallbacks, 0);
    }

    void deadlineAndCancellationStayBounded() {
        TimingFixture server;
        server.handler = [](QTcpSocket *, QByteArray, QByteArray) {};
        JsonHttpClient client;
        client.configure(server.url(), {}, 200);
        QSignalSpy measurements(&client, &JsonHttpClient::requestMeasured);
        int finished = 0;
        JsonHttpResult result;
        QVERIFY(client.request("status", "GET", "/blocked", {}, {}, {},
                               [&](const JsonHttpResult &r) { result = r; ++finished; }));
        QVERIFY(!client.request("status", "GET", "/duplicate", {}, {}, {}, [](const JsonHttpResult &) {}));
        QTRY_COMPARE(finished, 1);
        QCOMPARE(result.error, QString("request_timeout"));
        QCOMPARE(result.firstResponseMs, qint64(-1));
        QVERIFY(result.requestSentMs >= 0 && result.roundTripMs >= 180);
        QCOMPARE(measurements.count(), 1);
        QVERIFY(client.request("write", "POST", "/cancelled", {}, {}, {},
                               [&](const JsonHttpResult &) { ++finished; }));
        client.cancelAll();
        QTest::qWait(250);
        QCOMPARE(finished, 1);
        QCOMPARE(measurements.count(), 1);
    }

    void keepAliveReusesTheConnectionWithoutReplayingWrites() {
        TimingFixture server;
        int received = 0;
        server.handler = [&](QTcpSocket *socket, QByteArray, QByteArray) {
            ++received;
            TimingFixture::reply(socket);
        };
        JsonHttpClient client;
        client.configure(server.url(), {}, 2000);
        int finished = 0;
        for (int i = 0; i < 3; ++i) {
            QVERIFY(client.request("write", "POST", "/write", {{"number", i}}, {}, {},
                                   [&](const JsonHttpResult &r) { QVERIFY(r.ok()); ++finished; }));
            QTRY_COMPARE(finished, i + 1);
        }
        QCOMPARE(received, 3);
        QCOMPARE(server.connections, 1);
    }

    void measurementObserverCanCancelOrDestroyClient_data() {
        QTest::addColumn<bool>("destroy");
        QTest::newRow("cancel") << false;
        QTest::newRow("destroy") << true;
    }
    void measurementObserverCanCancelOrDestroyClient() {
        QFETCH(bool, destroy);
        TimingFixture server;
        server.handler = [](QTcpSocket *socket, QByteArray, QByteArray) { TimingFixture::reply(socket); };
        QPointer<JsonHttpClient> client = new JsonHttpClient;
        client->configure(server.url(), {}, 2000);
        int measured = 0, callbacks = 0;
        connect(client, &JsonHttpClient::requestMeasured, this, [&] {
            ++measured;
            if (destroy) delete client.data();
            else client->cancelAll();
        });
        QVERIFY(client->request("status", "GET", "/health", {}, {}, {},
                                [&](const JsonHttpResult &) { ++callbacks; }));
        QTRY_COMPARE(measured, 1);
        QTest::qWait(50);
        QCOMPARE(callbacks, 0);
        if (destroy) QVERIFY(client.isNull());
        delete client.data();
    }
};
QTEST_GUILESS_MAIN(JsonHttpClientTest)
#include "test_jsonhttpclient.moc"
