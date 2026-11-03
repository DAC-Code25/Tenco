#include "navigationdiagnosticspanel.h"
#include "trackingclient.h"

#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QPointer>
#include <QProgressBar>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

namespace {
QJsonObject input(const QString &status = "online", bool valid = true) {
    return {{"status", status}, {"valid", valid}, {"ageMs", 20}};
}
QJsonObject diagnostics() {
    auto rtk = input();
    rtk["satellites"] = 23;
    rtk["fixMode"] = "rtk_fixed";
    rtk["fixQuality"] = 4;
    return {{"schemaVersion", 1}, {"sampledAt", QDateTime::currentMSecsSinceEpoch() / 1000.0},
        {"sensors", QJsonObject{{"encoder", input()}, {"frontRtk", rtk}, {"rearRtk", rtk}, {"imu", input()}}},
        {"fusion", QJsonObject{{"finalHttp", input()}}},
        {"host", QJsonObject{{"cpu", QJsonObject{{"usagePercent", 31.2}}},
            {"gpu", QJsonObject{{"status", "available"}, {"usagePercent", 12.3}}},
            {"memory", QJsonObject{{"totalBytes", 8.0 * 1024 * 1024 * 1024},
                {"availableBytes", 6.0 * 1024 * 1024 * 1024}, {"usedPercent", 25}}},
            {"disk", QJsonObject{{"totalBytes", 256.0 * 1024 * 1024 * 1024},
                {"availableBytes", 128.0 * 1024 * 1024 * 1024}, {"usedPercent", 50}}},
            {"uptimeSeconds", 7200}}}};
}
class DiagnosticsFixture : public QTcpServer {
  public:
    QJsonObject sample = diagnostics();
    QJsonObject resources = diagnostics();
    QJsonObject health{{"apiVersion", 1}, {"bootId", "boot-1"}, {"ready", true}};
    QJsonObject status{{"apiVersion", 1}, {"bootId", "boot-1"}, {"state", "Paused"},
        {"autoResumePending", true}, {"recoveryStableMs", 0}};
    int diagnosticRequests = 0, healthRequests = 0, statusRequests = 0, resourceRequests = 0;
    int directStatusRequests = 0;
    bool fullTrackingStatus = false, frozenTrackingStatus = false;
    bool holdNextHealth = false;
    QPointer<QTcpSocket> heldHealth;
    QJsonObject heldHealthObject;
    quint64 trackingSeq = 0;
    int resourceCode = 200;
    int resourceDelayMs = 0;
    bool frozenResources = false;
    int diagnosticCode = 200, healthCode = 200, statusCode = 200, diagnosticDelayMs = 0;
    bool frozenSample = false;
    QList<QByteArray> methods, paths, authorizations;

    DiagnosticsFixture() {
        resources["apiVersion"] = 1;
        resources["status"] = "available";
        resources["stale"] = false;
        resources["snapshotAgeMs"] = 0;
        listen(QHostAddress::LocalHost);
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto *socket = nextPendingConnection();
                auto bytes = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, bytes] {
                    bytes->append(socket->readAll());
                    if (!bytes->contains("\r\n\r\n") || socket->property("handled").toBool())
                        return;
                    socket->setProperty("handled", true);
                    const auto parts = bytes->split('\n').first().split(' ');
                    methods.append(parts.value(0));
                    const auto path = parts.value(1).split('?').first();
                    paths.append(path);
                    for (const auto &line : bytes->split('\n'))
                        if (line.toLower().startsWith("authorization:"))
                            authorizations.append(line.mid(14).trimmed());
                    if (path == "/api/v1/diagnostics") {
                        ++diagnosticRequests;
                        if (!frozenSample)
                            sample["sampledAt"] = QDateTime::currentMSecsSinceEpoch() / 1000.0;
                        const QPointer<QTcpSocket> guarded(socket);
                        const auto body = sample;
                        const int code = diagnosticCode;
                        QTimer::singleShot(diagnosticDelayMs, this, [guarded, body, code] {
                            if (guarded && guarded->state() == QAbstractSocket::ConnectedState)
                                reply(guarded, body, code);
                        });
                    } else if (path == "/api/v1/management/resources") {
                        ++resourceRequests;
                        if (!frozenResources)
                            resources["sampledAt"] = QDateTime::currentMSecsSinceEpoch() / 1000.0;
                        const QPointer<QTcpSocket> guarded(socket);
                        const auto body = resources;
                        const int code = resourceCode;
                        QTimer::singleShot(resourceDelayMs, this, [guarded, body, code] {
                            if (guarded && guarded->state() == QAbstractSocket::ConnectedState)
                                reply(guarded, body, code);
                        });
                    } else if (path == "/api/v1/health") {
                        ++healthRequests;
                        auto object = health;
                        object["fixtureRequestNumber"] = healthRequests;
                        if (holdNextHealth) {
                            holdNextHealth = false; heldHealth = socket; heldHealthObject = object;
                            return;
                        }
                        reply(socket, object, healthCode);
                    } else if (path == "/api/v1/status") {
                        ++statusRequests;
                        if (!parts.value(1).contains('?')) ++directStatusRequests;
                        if (fullTrackingStatus) {
                            if (!frozenTrackingStatus) ++trackingSeq;
                            status.insert("seq", QString::number(trackingSeq));
                            status.insert("stateVersion", "1");
                            for (const auto *key : {"stepProgressMeters", "stepLengthMeters", "lateralErrorMeters", "headingErrorRad", "remainingAngleRad", "remainingWaitMs"})
                                status.insert(key, 0);
                            status.insert("measuredTwist", QJsonObject{{"v", 0}, {"omega", 0}});
                        }
                        reply(socket, status, statusCode);
                    } else {
                        reply(socket, {}, 404);
                    }
                });
            }
        });
    }
    QUrl url() const { return QUrl(QString("http://127.0.0.1:%1/api/v1").arg(serverPort())); }
    static void reply(QTcpSocket *socket, const QJsonObject &object, int code) {
        const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
        socket->write("HTTP/1.1 " + QByteArray::number(code) + " Result\r\nContent-Type: application/json\r\nContent-Length: " +
            QByteArray::number(bytes.size()) + "\r\nConnection: close\r\n\r\n" + bytes);
        socket->disconnectFromHost();
    }
};
QLabel *value(NavigationDiagnosticsPanel &panel, const char *key) {
    return panel.findChild<QLabel *>("navigationDiagnostics_" + QString::fromLatin1(key));
}
QLabel *detail(NavigationDiagnosticsPanel &panel, const char *key) {
    return panel.findChild<QLabel *>("navigationDiagnosticsDetail_" + QString::fromLatin1(key));
}
NavigationDiagnosticsConnection connection(const DiagnosticsFixture &server, int timeout = 500) {
    return {server.url(), server.url(), "diagnostics-test-token", timeout, timeout, true, true};
}
} // namespace

class NavigationDiagnosticsTest : public QObject {
    Q_OBJECT
  private slots:
    void sharedFreshHealthAvoidsDuplicateReadAndStaleHealthFallsBack() {
        DiagnosticsFixture server;
        server.fullTrackingStatus = true;
        const auto config = connection(server, 5000);
        TrackingClient client;
        client.configure(config.trackingUrl, config.token, config.trackingTimeoutMs);
        client.start();
        QTRY_VERIFY(client.healthFresh());
        NavigationDiagnosticsPanel panel;
        panel.configure(config);
        panel.setTrackingClient(&client);
        const auto initialReads = server.healthRequests;
        panel.show();
        QTRY_VERIFY(value(panel, "tracking")->text().contains("已就绪"));
        QTest::qWait(100);
        QCOMPARE(server.healthRequests, initialReads);
        panel.hide();
        server.holdNextHealth = true;
        QTRY_VERIFY(server.heldHealth);
        QTRY_VERIFY_WITH_TIMEOUT(!client.healthFresh(), 2300);
        QVERIFY(client.fresh());
        const auto beforeFallback = server.healthRequests;
        panel.show();
        QTRY_COMPARE(server.healthRequests, beforeFallback + 1);
        QTRY_VERIFY(value(panel, "tracking")->text().contains("已就绪"));
        QVERIFY(!client.healthFresh());
        const auto fallbackRequest = server.healthRequests;
        DiagnosticsFixture::reply(server.heldHealth, server.heldHealthObject, 200);
        // The held request outlasted the main client's polling interval. Its
        // overdue refresh is legitimate; wait for that new sample before
        // measuring whether reopening the panel adds its own health request.
        QTRY_VERIFY(client.health().value("fixtureRequestNumber").toInt() > fallbackRequest);
        QVERIFY(client.healthFresh());
        const auto beforeReuse = server.healthRequests;
        panel.configure(config);
        QTest::qWait(100);
        QCOMPARE(server.healthRequests, beforeReuse);
        for (const auto &method : server.methods) QCOMPARE(method, QByteArray("GET"));
    }
    void sharedTrackingStatusAvoidsDuplicateReadsAndPreservesRealAge() {
        DiagnosticsFixture server;
        server.fullTrackingStatus = true;
        const auto config = connection(server);
        TrackingClient client;
        client.configure(config.trackingUrl, config.token, config.trackingTimeoutMs);
        client.start();
        QTRY_VERIFY(client.fresh());
        NavigationDiagnosticsPanel panel;
        panel.configure(config);
        panel.setTrackingClient(&client);
        panel.show();
        QTRY_VERIFY(value(panel, "task")->text().contains("自动继续"));
        QCOMPARE(server.directStatusRequests, 0);
        server.frozenTrackingStatus = true;
        QTRY_VERIFY_WITH_TIMEOUT(value(panel, "task")->text().contains("状态延迟"), 3500);
        QCOMPARE(server.directStatusRequests, 0);
        QTRY_VERIFY_WITH_TIMEOUT(value(panel, "task")->text().contains("已过期"), 6000);
        server.frozenTrackingStatus = false;
        QTRY_VERIFY_WITH_TIMEOUT(value(panel, "task")->text().contains("自动继续") &&
            !value(panel, "task")->text().contains("状态延迟"), 3500);
        auto replacement = config;
        replacement.token = "different-connection-token";
        panel.configure(replacement);
        QTRY_VERIFY(value(panel, "task")->text().contains("已过期"));
        QCOMPARE(server.directStatusRequests, 0);
        panel.configure(config);
        QTRY_VERIFY(value(panel, "task")->text().contains("自动继续"));
        client.stop();
        QTRY_VERIFY_WITH_TIMEOUT(value(panel, "task")->text().contains("已过期"), 3000);
        QCOMPARE(server.directStatusRequests, 0);
        for (const auto &method : server.methods) QCOMPARE(method, QByteArray("GET"));
    }
    void resourcesRemainAvailableWhenFusionIsStopped() {
        DiagnosticsFixture server;
        server.diagnosticCode = server.healthCode = 503;
        auto config = connection(server);
        config.managementUrl = server.url();
        NavigationDiagnosticsPanel panel;
        panel.configure(config);
        panel.show();
        QTRY_VERIFY(detail(panel, "service")->text().contains("不可达"));
        QTRY_COMPARE(value(panel, "cpu")->text(), QStringLiteral("31.2 %"));
        QCOMPARE(value(panel, "gpu")->text(), QStringLiteral("12.3 %"));
        QVERIFY(value(panel, "encoder")->text().contains("未知"));
        QTest::qWait(2200);
        QVERIFY(server.resourceRequests >= 2);
        QCOMPARE(value(panel, "memory")->text(), QStringLiteral("25.0 %"));
        server.resourceCode = 401;
        QTRY_VERIFY_WITH_TIMEOUT(panel.findChild<QLabel *>("navigationResourcesState")->text().contains("认证失败"), 3000);
        QCOMPARE(value(panel, "cpu")->text(), QStringLiteral("N/A"));
        server.resourceCode = 200;
        QTRY_COMPARE_WITH_TIMEOUT(value(panel, "cpu")->text(), QStringLiteral("31.2 %"), 3000);
        panel.hide();
        const auto requests = server.resourceRequests;
        QTest::qWait(2200);
        QCOMPARE(server.resourceRequests, requests);
        QCOMPARE(value(panel, "cpu")->text(), QStringLiteral("N/A"));
        for (const auto &method : server.methods)
            QCOMPARE(method, QByteArray("GET"));
        for (const auto &authorization : server.authorizations)
            QCOMPARE(authorization, QByteArray("Bearer diagnostics-test-token"));
    }
    void expiredResourcesDoNotUseFusionValues() {
        DiagnosticsFixture server;
        server.frozenResources = true;
        auto config = connection(server);
        config.managementUrl = server.url();
        NavigationDiagnosticsPanel panel;
        panel.configure(config);
        panel.show();
        QTRY_COMPARE(value(panel, "cpu")->text(), QStringLiteral("31.2 %"));
        QTRY_VERIFY_WITH_TIMEOUT(panel.findChild<QLabel *>("navigationResourcesState")->text().contains("过期"), 7500);
        QCOMPARE(value(panel, "service")->text(), QStringLiteral("在线"));
        QCOMPARE(value(panel, "cpu")->text(), QStringLiteral("N/A"));
        server.frozenResources = false;
        QTRY_COMPARE_WITH_TIMEOUT(value(panel, "cpu")->text(), QStringLiteral("31.2 %"), 3000);
        server.resources["stale"] = true;
        QTRY_COMPARE_WITH_TIMEOUT(value(panel, "cpu")->text(), QStringLiteral("N/A"), 3000);
        server.resources["stale"] = false;
        server.resources["snapshotAgeMs"] = 6000;
        QTest::qWait(2200);
        QCOMPARE(value(panel, "cpu")->text(), QStringLiteral("N/A"));
        server.resourceCode = 404;
        QTRY_VERIFY_WITH_TIMEOUT(panel.findChild<QLabel *>("navigationResourcesState")->text().contains("未安装"), 3000);
    }
    void resourceDelayAndConnectionChanges() {
        DiagnosticsFixture oldServer, nextServer;
        oldServer.resourceDelayMs = 800;
        oldServer.resources["snapshotAgeMs"] = 4500;
        auto config = connection(oldServer, 10000);
        config.managementUrl = oldServer.url();
        NavigationDiagnosticsPanel panel;
        panel.configure(config);
        panel.show();
        QTRY_VERIFY_WITH_TIMEOUT(panel.findChild<QLabel *>("navigationResourcesState")->text().contains("过期"), 2500);
        QCOMPARE(value(panel, "cpu")->text(), QStringLiteral("N/A"));
        oldServer.resourceDelayMs = 10000;
        panel.configure(config);
        QTRY_COMPARE(oldServer.resourceRequests, 2);
        QTest::qWait(2200);
        QCOMPARE(oldServer.resourceRequests, 2);
        config.managementUrl = nextServer.url();
        config.token = "replacement-resource-token";
        panel.configure(config);
        QTRY_COMPARE(value(panel, "cpu")->text(), QStringLiteral("31.2 %"));
        QCOMPARE(nextServer.resourceRequests, 1);
        QCOMPARE(nextServer.authorizations.first(), QByteArray("Bearer replacement-resource-token"));
        panel.hide();
        QTest::qWait(2200);
        QCOMPARE(nextServer.resourceRequests, 1);
    }
    void realIpcResourcesWithoutFusion() {
        const auto url = qEnvironmentVariable("TENCO_RESOURCES_URL");
        if (url.isEmpty())
            QSKIP("Set management URL for read-only IPC resource validation");
        NavigationDiagnosticsConnection config;
        config.poseEnabled = config.trackingEnabled = false;
        config.managementUrl = QUrl(url);
        config.token = qEnvironmentVariable("TENCO_DIAGNOSTICS_TOKEN");
        NavigationDiagnosticsPanel panel;
        panel.resize(1050, 850);
        panel.configure(config);
        panel.show();
        QTRY_VERIFY_WITH_TIMEOUT(value(panel, "cpu")->text() != "N/A", 10000);
        QTest::qWait(6200);
        for (const auto *key : {"cpu", "gpu", "memory", "disk"}) {
            QVERIFY2(value(panel, key)->text() != "N/A", key);
            qInfo().noquote() << key << value(panel, key)->text();
        }
        QVERIFY(value(panel, "encoder")->text().contains("未知"));
        const auto screenshot = qEnvironmentVariable("TENCO_DIAGNOSTICS_REAL_SCREENSHOT");
        if (!screenshot.isEmpty())
            QVERIFY(panel.grab().save(screenshot));
    }
    void showsSensorsResourcesAndRecoveryWithoutWrites() {
        DiagnosticsFixture server;
        NavigationDiagnosticsPanel panel;
        panel.resize(1000, 890);
        panel.configure(connection(server));
        QTest::qWait(100);
        QCOMPARE(server.diagnosticRequests, 0);
        panel.show();
        QTRY_COMPARE(value(panel, "service")->text(), QStringLiteral("在线"));
        QTRY_VERIFY(value(panel, "task")->text().contains("自动继续"));
        QCOMPARE(value(panel, "encoder")->property("statusTone").toString(), QStringLiteral("ok"));
        QCOMPARE(value(panel, "frontRtk")->text(), QStringLiteral("在线"));
        QVERIFY(detail(panel, "frontRtk")->text().contains("卫星 23"));
        QVERIFY(detail(panel, "rearRtk")->text().contains("RTK 固定解"));
        QCOMPARE(value(panel, "cpu")->text(), QStringLiteral("31.2 %"));
        QCOMPARE(value(panel, "gpu")->text(), QStringLiteral("12.3 %"));
        QCOMPARE(value(panel, "memory")->text(), QStringLiteral("25.0 %"));
        QCOMPARE(value(panel, "disk")->text(), QStringLiteral("50.0 %"));
        QVERIFY(detail(panel, "memory")->text().contains("6.00 / 8.00 GiB"));
        QVERIFY(detail(panel, "disk")->text().contains("128.00 / 256.00 GiB"));
        const auto screenshot = qEnvironmentVariable("TENCO_DIAGNOSTICS_SCREENSHOT");
        if (!screenshot.isEmpty())
            QVERIFY(panel.grab().save(screenshot));

        auto sensors = server.sample.value("sensors").toObject();
        sensors["encoder"] = input("offline", false);
        auto front = input("online", false);
        front["satellites"] = QJsonValue();
        front["fixMode"] = "invalid";
        sensors["frontRtk"] = front;
        sensors["imu"] = QJsonObject{{"status", "unknown"}, {"ageMs", QJsonValue()}};
        server.sample["sensors"] = sensors;
        auto host = server.sample.value("host").toObject();
        host["cpu"] = QJsonObject{{"usagePercent", QJsonValue()}};
        host["gpu"] = QJsonObject{{"status", "unavailable"}, {"usagePercent", QJsonValue()}};
        server.sample["host"] = host;
        server.status["autoResumePending"] = false;
        QTRY_VERIFY_WITH_TIMEOUT(value(panel, "encoder")->text().startsWith("离线"), 3000);
        QCOMPARE(value(panel, "encoder")->property("statusTone").toString(), QStringLiteral("error"));
        QCOMPARE(value(panel, "frontRtk")->text(), QStringLiteral("数据异常"));
        QVERIFY(detail(panel, "frontRtk")->text().contains("卫星 N/A"));
        QCOMPARE(value(panel, "frontRtk")->property("statusTone").toString(), QStringLiteral("error"));
        QCOMPARE(value(panel, "imu")->text(), QStringLiteral("未知"));
        QCOMPARE(value(panel, "cpu")->text(), QStringLiteral("N/A"));
        QVERIFY(value(panel, "gpu")->text().startsWith("N/A"));
        QTRY_VERIFY(value(panel, "task")->text().contains("显式继续"));
        QVERIFY(!server.methods.isEmpty());
        for (const auto &method : server.methods)
            QCOMPARE(method, QByteArray("GET"));
        QCOMPARE(server.methods.size(), server.authorizations.size());
        for (const auto &auth : server.authorizations)
            QCOMPARE(auth, QByteArray("Bearer diagnostics-test-token"));
    }
    void errorsClearOldOnlineAndRecover() {
        DiagnosticsFixture server;
        NavigationDiagnosticsPanel panel;
        panel.configure(connection(server));
        panel.show();
        QTRY_COMPARE(value(panel, "service")->text(), QStringLiteral("在线"));
        server.diagnosticCode = 503;
        server.healthCode = 503;
        QTRY_VERIFY_WITH_TIMEOUT(detail(panel, "service")->text().contains("不可达"), 3000);
        QVERIFY(value(panel, "encoder")->text().contains("未知"));
        QVERIFY(value(panel, "encoder")->property("statusTone").toString().isEmpty());
        QCOMPARE(value(panel, "cpu")->text(), QStringLiteral("N/A"));
        QVERIFY(!detail(panel, "memory")->text().contains("6.00"));
        for (auto *bar : panel.findChildren<QProgressBar *>()) {
            QCOMPARE(bar->value(), 0);
            QVERIFY(!bar->isEnabled());
        }
        QTRY_VERIFY(detail(panel, "tracking")->text().contains("不可达"));
        server.diagnosticCode = server.healthCode = 200;
        server.health["ready"] = false;
        server.status["state"] = "Tracking";
        server.status["autoResumePending"] = false;
        QTRY_COMPARE_WITH_TIMEOUT(value(panel, "service")->text(), QStringLiteral("在线"), 3000);
        QTRY_COMPARE(value(panel, "tracking")->text(), QStringLiteral("在线 · 未就绪"));
        QTRY_COMPARE(value(panel, "task")->text(), QStringLiteral("跟踪中"));
        server.sample["schemaVersion"] = 2;
        QTRY_VERIFY_WITH_TIMEOUT(detail(panel, "service")->text().contains("响应无效"), 3000);
        QVERIFY(value(panel, "fusion")->property("statusTone").toString().isEmpty());
        server.diagnosticCode = 404;
        QTRY_VERIFY_WITH_TIMEOUT(detail(panel, "service")->text().contains("不支持"), 3000);
    }
    void visibilityCancellationAndNoOverlappingPolls() {
        DiagnosticsFixture server;
        server.diagnosticDelayMs = 10000;
        auto panel = std::make_unique<NavigationDiagnosticsPanel>();
        panel->configure(connection(server, 10000));
        panel->show();
        QTRY_COMPARE(server.diagnosticRequests, 1);
        QTest::qWait(4200);
        QCOMPARE(server.diagnosticRequests, 1);
        panel->hide();
        const auto healthCount = server.healthRequests;
        const auto statusCount = server.statusRequests;
        QTest::qWait(2200);
        QCOMPARE(server.diagnosticRequests, 1);
        QCOMPARE(server.healthRequests, healthCount);
        QCOMPARE(server.statusRequests, statusCount);
        server.diagnosticDelayMs = 0;
        panel->show();
        QTRY_COMPARE(server.diagnosticRequests, 2);
        QTRY_COMPARE(value(*panel, "service")->text(), QStringLiteral("在线"));
        server.diagnosticDelayMs = 10000;
        panel->configure(connection(server, 10000));
        QTRY_COMPARE(server.diagnosticRequests, 3);
        panel.reset();
        QTest::qWait(100);
    }
    void timeoutThenConfigurationChangeIgnoresOldReply() {
        DiagnosticsFixture oldServer, nextServer;
        oldServer.diagnosticDelayMs = 1000;
        NavigationDiagnosticsPanel panel;
        panel.configure(connection(oldServer, 200));
        panel.show();
        QTRY_VERIFY(detail(panel, "service")->text().contains("不可达"));
        nextServer.health["ready"] = false;
        auto next = connection(nextServer);
        next.token = "replacement-test-token";
        panel.configure(next);
        QTRY_COMPARE(value(panel, "service")->text(), QStringLiteral("在线"));
        QTRY_COMPARE(value(panel, "tracking")->text(), QStringLiteral("在线 · 未就绪"));
        QTest::qWait(1000);
        QCOMPARE(value(panel, "tracking")->text(), QStringLiteral("在线 · 未就绪"));
        for (const auto &auth : nextServer.authorizations)
            QCOMPARE(auth, QByteArray("Bearer replacement-test-token"));
        next.poseEnabled = next.trackingEnabled = false;
        panel.configure(next);
        const auto requests = nextServer.methods.size();
        QTest::qWait(2200);
        QCOMPARE(nextServer.methods.size(), requests);
        QVERIFY(value(panel, "encoder")->text().contains("未知"));
        QVERIFY(detail(panel, "tracking")->text().contains("已禁用"));
    }
    void frozenAndExpiredSnapshotsCannotStayGreen() {
        DiagnosticsFixture server;
        server.frozenSample = true;
        NavigationDiagnosticsPanel panel;
        panel.configure(connection(server));
        panel.show();
        QTRY_COMPARE(value(panel, "service")->text(), QStringLiteral("在线"));
        QTRY_VERIFY_WITH_TIMEOUT(detail(panel, "service")->text().contains("过期"), 7500);
        QVERIFY(server.diagnosticRequests >= 3);
        QVERIFY(value(panel, "encoder")->property("statusTone").toString().isEmpty());
        server.frozenSample = false;
        QTRY_COMPARE_WITH_TIMEOUT(value(panel, "service")->text(), QStringLiteral("在线"), 3000);
        server.sample["snapshotAgeMs"] = 6000;
        QTRY_VERIFY_WITH_TIMEOUT(detail(panel, "service")->text().contains("过期"), 3000);
        QCOMPARE(value(panel, "cpu")->text(), QStringLiteral("N/A"));
        server.sample["snapshotAgeMs"] = 100;
        QTRY_COMPARE_WITH_TIMEOUT(value(panel, "service")->text(), QStringLiteral("在线"), 3000);
    }
    void realIpcReadOnlyIntegration() {
        const auto poseUrl = qEnvironmentVariable("TENCO_DIAGNOSTICS_POSE_URL");
        const auto trackingUrl = qEnvironmentVariable("TENCO_DIAGNOSTICS_TRACKING_URL");
        if (poseUrl.isEmpty() || trackingUrl.isEmpty())
            QSKIP("Set diagnostic pose/tracking URLs for scheduled read-only IPC validation");
        NavigationDiagnosticsPanel panel;
        panel.resize(1000, 890);
        panel.configure({QUrl(poseUrl), QUrl(trackingUrl), qEnvironmentVariable("TENCO_DIAGNOSTICS_TOKEN"), 3000, 3000});
        panel.show();
        QTRY_COMPARE_WITH_TIMEOUT(value(panel, "service")->text(), QStringLiteral("在线"), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(value(panel, "tracking")->text().startsWith("在线"), 10000);
        QTest::qWait(6200);
        QCOMPARE(value(panel, "service")->text(), QStringLiteral("在线"));
        QVERIFY(value(panel, "tracking")->text().startsWith("在线"));
        const auto screenshot = qEnvironmentVariable("TENCO_DIAGNOSTICS_REAL_SCREENSHOT");
        if (!screenshot.isEmpty())
            QVERIFY(panel.grab().save(screenshot));
        QJsonObject result;
        for (const auto *key : {"service", "fusion", "tracking", "task", "encoder", "frontRtk", "rearRtk", "imu", "cpu", "gpu", "memory", "disk", "uptime", "sampledAt"}) {
            result[key] = value(panel, key)->text();
            qInfo().noquote() << key << value(panel, key)->text();
        }
        const auto output = qEnvironmentVariable("TENCO_DIAGNOSTICS_REAL_RESULT");
        if (!output.isEmpty()) {
            QFile file(output);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(QJsonDocument(result).toJson());
        }
    }
};

QTEST_MAIN(NavigationDiagnosticsTest)
#include "test_navigation_diagnostics.moc"
