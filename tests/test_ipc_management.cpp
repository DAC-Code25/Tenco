#include "ipcmanagementpanel.h"
#include "trackingclient.h"
#include "supportpagestyle.h"
#include <QApplication>
#include <QAbstractItemView>
#include <QComboBox>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QTabWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QVBoxLayout>
#include <QtTest>

class ManagementFixture : public QTcpServer {
public:
    int reads = 0, writes = 0, operationReads = 0;
    int backupReads = 0, backupWrites = 0;
    int originReads = 0;
    bool originEndpointAvailable = true;
    bool malformedBackupReply = false;
    bool delayNextBackup = false;
    QPointer<QTcpSocket> delayedBackupSocket;
    QJsonObject delayedBackupObject;
    bool delayNextManagement = false;
    QPointer<QTcpSocket> delayedManagementSocket;
    QJsonObject delayedManagementObject;
    int delayedManagementDisconnects = 0;
    QString revision = "1", parameterState = "applied";
    QString operationState = "applied", managementBoot = "test-process";
    int operationStatus = 200;
    QHash<QString, QString> serviceStates{{"fusion", "active"}, {"tracking", "inactive"}, {"camera", "active"}};
    QString fusionProfile = "isolated", originRevision = "origin-config-1";
    QJsonObject savedOrigin{{"originRevision", "field-origin-1"}, {"latitude", 30.222814812345}, {"longitude", 120.022578112345}, {"altitude", 0}};
    bool includeOrigin = true, originConsistent = true;
    QString lastPath, lastAuthorization;
    QJsonObject lastBody;
    double speed = 0.2;
    QJsonArray customFields;
    bool runtimeRevisionAvailable = true;
    bool dropWriteReply = false, reject = false;
    QString navigationState = "Ready";
    bool autoResume = false;
    quint64 navigationSeq = 0;
    int heartbeats = 0, sessionRequests = 0;
    ManagementFixture() {
        listen(QHostAddress::LocalHost);
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto *socket = nextPendingConnection();
                auto bytes = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, bytes] {
                    bytes->append(socket->readAll());
                    const auto end = bytes->indexOf("\r\n\r\n");
                    if (end < 0 || socket->property("handled").toBool()) return;
                    const auto header = bytes->left(end);
                    int length = 0;
                    for (const auto &line : header.split('\n')) {
                        if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toInt();
                        if (line.toLower().startsWith("authorization:")) lastAuthorization = QString::fromUtf8(line.mid(14).trimmed());
                    }
                    if (bytes->size() < end + 4 + length) return;
                    socket->setProperty("handled", true);
                    const auto line = header.split('\n').first().split(' ');
                    const auto method = line.value(0), path = line.value(1);
                    if (method == "GET" && path.startsWith("/api/v1/status")) {
                        QJsonObject status{{"apiVersion", 1}, {"seq", QString::number(++navigationSeq)}, {"bootId", "nav-test"},
                            {"stateVersion", "1"}, {"lastEventSeq", "0"}, {"state", navigationState}, {"taskId", "task"}, {"taskRevision", 1},
                            {"measuredTwist", QJsonObject{{"v", 0}, {"omega", 0}}}, {"autoResumePending", autoResume}, {"recoveryStableMs", 0}};
                        for (const auto *key : {"stepProgressMeters", "stepLengthMeters", "lateralErrorMeters", "headingErrorRad", "remainingAngleRad", "remainingWaitMs"}) status.insert(key, 0);
                        reply(socket, status);
                    } else if (path == "/api/v1/sessions") {
                        ++sessionRequests;
                        reply(socket, {{"apiVersion", 1}, {"bootId", "nav-test"}, {"sessionId", "test-session"}, {"sessionToken", "test-token"}, {"expiresInMs", 1000}});
                    } else if (path.endsWith("/heartbeat")) {
                        ++heartbeats;
                        reply(socket, {{"apiVersion", 1}, {"bootId", "nav-test"}, {"sessionId", "test-session"}, {"stateVersion", "1"}, {"motionPermit", "test-permit"}, {"permitValidForMs", 500}});
                    } else if (path == "/api/v1/health" || path == "/api/v1/configuration") {
                        reply(socket, {{"apiVersion", 1}, {"bootId", "nav-test"}, {"ready", true}});
                    } else if (method == "GET" && path == "/api/v1/management") {
                        ++reads;
                        if (delayNextManagement) {
                            delayNextManagement = false;
                            delayedManagementSocket = socket;
                            delayedManagementObject = snapshot();
                            connect(socket, &QTcpSocket::disconnected, this, [this] { ++delayedManagementDisconnects; });
                        } else reply(socket, snapshot());
                    } else if (method == "GET" && path == "/api/v1/management/origin") {
                        ++originReads;
                        if (!originEndpointAvailable) reply(socket, {}, 404);
                        else reply(socket, {{"apiVersion", 1}, {"bootId", managementBoot}, {"origin", snapshot().value("origin")}});
                    } else if (method == "GET" && path == "/api/v1/management/backups") {
                        ++backupReads;
                        if (malformedBackupReply) { reply(socket, {}); return; }
                        QJsonArray backups;
                        if (backupWrites) backups.append(QJsonObject{{"backupId", "test-backup-001"}, {"createdAtUtc", "2026-10-27T10:00:00Z"}, {"fileCount", 5}});
                        QJsonObject value{{"apiVersion", 1}, {"backupRoot", "backups/config"}, {"backups", backups}};
                        if (delayNextBackup) { delayNextBackup = false; delayedBackupSocket = socket; delayedBackupObject = value; }
                        else reply(socket, value);
                    } else if (method == "POST" && path == "/api/v1/management/backups") {
                        ++writes; ++backupWrites; lastPath = QString::fromUtf8(path);
                        lastBody = QJsonDocument::fromJson(bytes->mid(end + 4, length)).object();
                        if (dropWriteReply) socket->disconnectFromHost();
                        else reply(socket, {{"requestId", lastBody.value("requestId")}, {"state", "pending"}}, 202);
                    } else if (method == "POST") {
                        ++writes; lastPath = QString::fromUtf8(path);
                        lastBody = QJsonDocument::fromJson(bytes->mid(end + 4, length)).object();
                        if (reject) { reply(socket, {{"message", "请停止任务控制并等待会话释放后重试"}}, 409); return; }
                        const bool fusionStart = lastPath.endsWith("/services/fusion") && lastBody.value("action") != "stop";
                        const bool fusionApply = lastPath.contains("/parameters/fusion") && (lastBody.value("apply").toBool() || lastPath.endsWith("/apply"));
                        if (fusionStart || fusionApply) {
                            const auto origin = lastBody.value("origin").toObject();
                            if (lastBody.value("expectedOriginRevision") != originRevision || !origin.value("latitude").isDouble() || !origin.value("longitude").isDouble() || origin.value("altitude").toDouble(-1) != 0) {
                                reply(socket, {{"message", "本次基准站坐标未确认"}}, 409); return;
                            }
                            savedOrigin.insert("latitude", origin.value("latitude")); savedOrigin.insert("longitude", origin.value("longitude"));
                            savedOrigin.insert("altitude", 0); originConsistent = true;
                        }
                        if (lastPath.contains("parameters")) {
                            if (lastBody.value("expectedRevision") != revision) { reply(socket, {{"message", "配置版本冲突"}}, 409); return; }
                            if (!lastBody.value("values").toObject().isEmpty()) speed = lastBody.value("values").toObject().value("speed.limit").toDouble();
                            revision = QString::number(revision.toInt() + 1);
                            ++backupWrites;
                            parameterState = lastBody.value("apply").toBool() || lastPath.endsWith("/apply") ? "applied" : "pending";
                            if (parameterState == "applied") serviceStates[lastPath.contains("/fusion") ? "fusion" : "tracking"] = "active";
                        } else serviceStates[lastPath.section('/', -1)] = lastBody.value("action") == "stop" ? "inactive" : "active";
                        if (dropWriteReply) socket->disconnectFromHost();
                        else reply(socket, {{"requestId", lastBody.value("requestId")}, {"state", "pending"}}, 202);
                    } else if (method == "GET" && path.startsWith("/api/v1/management/operations/")) {
                        ++operationReads;
                        if (operationStatus == 404)
                            reply(socket, {{"code", "operation_not_found"}, {"message", "未找到此操作记录；请读取当前状态确认后重试"}}, 404);
                        else reply(socket, {{"requestId", lastBody.value("requestId")}, {"state", operationState}, {"message", "已完成并核验"}}, operationStatus);
                    } else reply(socket, {}, 404);
                });
            }
        });
    }
    QUrl url() const { return QUrl(QString("http://127.0.0.1:%1/api/v1").arg(serverPort())); }
    void releaseManagement() {
        const auto socket = delayedManagementSocket;
        delayedManagementSocket.clear();
        if (socket && socket->state() == QAbstractSocket::ConnectedState) reply(socket, delayedManagementObject);
    }
    QJsonObject snapshot() const {
        QJsonArray services;
        for (const auto &id : {"fusion", "tracking", "camera"}) services.append(QJsonObject{{"id", id}, {"activeState", serviceStates.value(id)}, {"canOperate", true}, {"actualProfile", QString(id) == "fusion" ? fusionProfile : QString("isolated")}});
        QJsonArray parameters;
        for (const auto &id : {"fusion", "tracking"}) parameters.append(QJsonObject{{"component", id}, {"revision", revision}, {"savedRevision", revision},
            {"appliedRevision", runtimeRevisionAvailable ? (parameterState == "applied" ? revision : QString("1")) : QString()}, {"state", parameterState}, {"message", "测试参数"},
            {"configPath", QString(id) == "fusion" ? "config/fusion/isolated.yaml" : "config/isolated/tracking.yaml"},
            {"fields", customFields.isEmpty() ? QJsonArray{QJsonObject{{"key", "speed.limit"}, {"label", "速度上限"}, {"type", "number"}, {"minimum", 0.1}, {"maximum", 1.5}, {"unit", "m/s"}, {"value", speed}}} : customFields}});
        QJsonObject result{{"apiVersion", 1}, {"bootId", managementBoot}, {"projectRoot", "/test/tenco-project"}, {"profile", "isolated"}, {"configuredProfile", "isolated"}, {"services", services}, {"parameters", parameters}};
        if (includeOrigin) result.insert("origin", QJsonObject{{"revision", originRevision}, {"saved", savedOrigin}, {"consistent", originConsistent}, {"message", originConsistent ? "融合与导航保存原点一致" : "原点配置不一致，启动确认可修复"}});
        return result;
    }
    static void reply(QTcpSocket *socket, const QJsonObject &object, int status = 200) {
        const auto body = QJsonDocument(object).toJson(QJsonDocument::Compact);
        socket->write("HTTP/1.1 " + QByteArray::number(status) + " Result\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
    }
};
class EnabledChangeRecorder : public QObject {
public:
    int count = 0;
    void watch(QWidget *widget) { widget->installEventFilter(this); }
protected:
    bool eventFilter(QObject *object, QEvent *event) override {
        if (event->type() == QEvent::EnabledChange) ++count;
        return QObject::eventFilter(object, event);
    }
};
class IpcManagementTest : public QObject {
    Q_OBJECT
private:
    static void connectPanel(IpcManagementPanel &panel, ManagementFixture &fixture) {
        SupportPageStyle::apply(&panel);
        panel.setConnectionProvider([&fixture] { return IpcManagementConnection{fixture.url(), "test-token", 500}; });
        panel.resize(1050, 850); panel.show();
    }
    static void confirmOrigin(IpcManagementPanel &panel) {
        auto *dialog = panel.findChild<QDialog *>("ipcFusionOriginDialog");
        QVERIFY(dialog); QVERIFY(dialog->isVisible());
        auto *confirm = dialog->findChild<QPushButton *>("ipcOriginConfirm");
        QVERIFY(confirm); confirm->click();
    }
private slots:
    void hiddenOriginRefreshAvoidsParametersAndDoesNotEnableOperations() {
        ManagementFixture fixture;
        IpcManagementPanel panel;
        panel.setConnectionProvider([&fixture] { return IpcManagementConnection{fixture.url(), "test-token", 500}; });
        QSignalSpy origins(&panel, &IpcManagementPanel::originSnapshotChanged);
        panel.refreshOriginSnapshot();
        QTRY_COMPARE(fixture.originReads, 1);
        QTRY_VERIFY(!origins.isEmpty() && origins.last().at(0).toJsonObject().value("revision") == fixture.originRevision);
        QCOMPARE(fixture.reads, 0);
        QCOMPARE(fixture.backupReads, 0);
        QCOMPARE(fixture.writes, 0);
        QVERIFY(!panel.findChild<QPushButton *>("ipcService_tracking_start")->isEnabled());
        panel.show();
        QTRY_COMPARE(fixture.reads, 1);
        QTRY_VERIFY(panel.findChild<QPushButton *>("ipcService_tracking_start")->isEnabled());
        panel.hide();
        fixture.originRevision = "origin-config-2";
        panel.refreshOriginSnapshot();
        QTRY_COMPARE(fixture.originReads, 2);
        QTRY_COMPARE(origins.last().at(0).toJsonObject().value("revision").toString(), fixture.originRevision);
        QCOMPARE(fixture.reads, 1);
    }
    void hiddenOriginRefreshSupportsOlderManagementService() {
        ManagementFixture fixture; fixture.originEndpointAvailable = false;
        IpcManagementPanel panel;
        panel.setConnectionProvider([&fixture] { return IpcManagementConnection{fixture.url(), "test-token", 500}; });
        QSignalSpy origins(&panel, &IpcManagementPanel::originSnapshotChanged);
        panel.refreshOriginSnapshot();
        QTRY_COMPARE(fixture.reads, 1);
        QTRY_VERIFY(!origins.isEmpty() && origins.last().at(0).toJsonObject().value("revision") == fixture.originRevision);
        QCOMPARE(fixture.originReads, 1);
        QCOMPARE(fixture.backupReads, 0);
        QVERIFY(!panel.findChild<QPushButton *>("ipcService_tracking_start")->isEnabled());
        panel.refreshOriginSnapshot();
        QTRY_COMPARE(fixture.reads, 2);
        QCOMPARE(fixture.originReads, 1);
        QCOMPARE(fixture.writes, 0);
    }
    void hiddenOriginRefreshStillQueriesPendingMaintenanceWithoutReplay() {
        ManagementFixture fixture; fixture.dropWriteReply = true;
        IpcManagementPanel panel; connectPanel(panel, fixture);
        auto *backup = panel.findChild<QPushButton *>("ipcBackupCreate");
        QTRY_VERIFY(backup->isEnabled());
        backup->click();
        QTRY_COMPARE(fixture.writes, 1);
        panel.hide();
        panel.refreshOriginSnapshot();
        QTRY_COMPARE(fixture.operationReads, 1);
        QTRY_VERIFY(panel.findChild<QLabel *>("ipcManagementResult")->text().contains("已完成并核验"));
        const auto reads = fixture.reads;
        panel.refreshOriginSnapshot();
        QTRY_COMPARE(fixture.reads, reads + 1);
        QCOMPARE(fixture.writes, 1);
        QCOMPARE(fixture.originReads, 0);
    }
    void backgroundSnapshotKeepsSelectorFocusPopupAndScroll_data() {
        QTest::addColumn<bool>("popup");
        QTest::newRow("focused-selector") << false;
        QTest::newRow("open-dropdown") << true;
    }
    void backgroundSnapshotKeepsSelectorFocusPopupAndScroll() {
        QFETCH(bool, popup);
        ManagementFixture fixture;
        QScrollArea scroll;
        scroll.resize(1000, 440);
        scroll.setWidgetResizable(true);
        auto *content = new QWidget;
        auto *layout = new QVBoxLayout(content);
        auto *panel = new IpcManagementPanel(content);
        SupportPageStyle::apply(content);
        panel->setConnectionProvider([&fixture] { return IpcManagementConnection{fixture.url(), "test-token", 4000}; });
        layout->addWidget(panel);
        auto *cameraScope = new QLabel("Camera settings below management", content);
        cameraScope->setMinimumHeight(200);
        layout->addWidget(cameraScope);
        auto *camera = new QLineEdit(content);
        camera->setObjectName("cameraFocusTarget");
        layout->addWidget(camera);
        scroll.setWidget(content);
        scroll.show(); scroll.activateWindow();
        auto *selector = panel->findChild<QComboBox *>("ipcParameterComponent");
        auto *pages = panel->findChild<QTabWidget *>("ipcMaintenanceTabs");
        QVERIFY(pages);
        pages->setCurrentIndex(1);
        auto *read = panel->findChild<QPushButton *>("ipcParametersRead");
        auto *start = panel->findChild<QPushButton *>("ipcService_tracking_start");
        auto *backup = panel->findChild<QPushButton *>("ipcBackupCreate");
        QTRY_VERIFY_WITH_TIMEOUT(start->isEnabled() && backup->isEnabled(), 3000);
        QTest::qWait(80); // Let the first snapshot finish layout and backup rendering.
        scroll.ensureWidgetVisible(selector, 0, 20);
        selector->setFocus(Qt::MouseFocusReason);
        QTRY_VERIFY(selector->hasFocus());
        QTest::qWait(30);
        const int position = scroll.verticalScrollBar()->value();
        QVERIFY(position < scroll.verticalScrollBar()->maximum());
        EnabledChangeRecorder changes;
        for (auto *widget : QList<QWidget *>{selector, read, start, backup}) changes.watch(widget);
        if (popup) { selector->showPopup(); QTRY_VERIFY(selector->view()->isVisible()); }
        for (int cycle = 0; cycle < 2; ++cycle) {
            fixture.delayNextManagement = true;
            // First cross a real periodic refresh with the popup/focus held;
            // then exercise an explicit snapshot refresh on the same controls.
            if (cycle != 0) panel->refreshOriginSnapshot();
            QTRY_VERIFY_WITH_TIMEOUT(fixture.delayedManagementSocket, 4000);
            QCOMPARE(scroll.verticalScrollBar()->value(), position);
            QCOMPARE(changes.count, 0);
            QVERIFY(selector->isEnabled()); QVERIFY(read->isEnabled()); QVERIFY(start->isEnabled());
            if (popup) QVERIFY(selector->view()->isVisible());
            else QVERIFY(selector->hasFocus());
            fixture.releaseManagement();
            QTest::qWait(80);
            QCOMPARE(changes.count, 0);
            QCOMPARE(scroll.verticalScrollBar()->value(), position);
            if (popup) QVERIFY(selector->view()->isVisible());
            else QVERIFY(selector->hasFocus());
        }
        if (popup) {
            QTest::keyClick(selector->view(), Qt::Key_Down);
            QTest::keyClick(selector->view(), Qt::Key_Return);
            QTRY_COMPARE(selector->currentData().toString(), QString("tracking"));
        }
        QCOMPARE(fixture.writes, 0);
        selector->hidePopup(); scroll.hide();
    }
    void explicitOperationPreemptsBackgroundSnapshotExactlyOnce_data() {
        QTest::addColumn<bool>("saveParameters");
        QTest::newRow("save-parameters") << true;
        QTest::newRow("stop-camera-service") << false;
    }
    void explicitOperationPreemptsBackgroundSnapshotExactlyOnce() {
        QFETCH(bool, saveParameters);
        ManagementFixture fixture;
        IpcManagementPanel panel; connectPanel(panel, fixture);
        auto *read = panel.findChild<QPushButton *>("ipcParametersRead");
        QTRY_VERIFY(fixture.reads > 0 && read->isEnabled());
        read->click();
        QTRY_VERIFY(panel.findChild<QDoubleSpinBox *>("ipcParameter_speed.limit"));
        auto *speed = panel.findChild<QDoubleSpinBox *>("ipcParameter_speed.limit");
        if (saveParameters) speed->setValue(0.4);
        auto *button = panel.findChild<QPushButton *>(saveParameters ? "ipcParametersSave" : "ipcService_camera_stop");
        QTRY_VERIFY(button->isEnabled());
        fixture.delayNextManagement = true;
        panel.refreshOriginSnapshot();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.delayedManagementSocket, 1000);
        QCOMPARE(fixture.delayedManagementObject.value("parameters").toArray().first().toObject().value("revision").toString(), QString("1"));
        QVERIFY(button->isEnabled());
        button->click(); button->click();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.writes, 1, 1000);
        QVERIFY(!button->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(fixture.delayedManagementDisconnects > 0, 1000);
        // Offer the old response if its socket is still usable; an invalidated
        // observer must never overwrite the result of the explicit operation.
        fixture.releaseManagement();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.operationReads > 0, 3000);
        if (saveParameters) {
            QCOMPARE(fixture.lastPath, QString("/api/v1/management/parameters/fusion"));
            QTRY_VERIFY_WITH_TIMEOUT(panel.findChild<QLabel *>("ipcParameterState")->text().contains("配置版本 2"), 3000);
            auto *updated = panel.findChild<QDoubleSpinBox *>("ipcParameter_speed.limit");
            QVERIFY(updated); QCOMPARE(updated->value(), 0.4);
            QVERIFY(!panel.findChild<QLabel *>("ipcParameterState")->text().contains("重新读取"));
        } else {
            QCOMPARE(fixture.lastPath, QString("/api/v1/management/services/camera"));
            QTRY_VERIFY_WITH_TIMEOUT(panel.findChild<QLabel *>("ipcServiceState_camera")->text().contains("已停止"), 3000);
        }
        QTest::qWait(100);
        QCOMPARE(fixture.writes, 1);
    }
    void explicitParameterReadLocksComponentAndWriteActions_data() {
        QTest::addColumn<bool>("backgroundInFlight");
        QTest::newRow("new-read") << false;
        QTest::newRow("read-during-background-snapshot") << true;
    }
    void explicitParameterReadLocksComponentAndWriteActions() {
        QFETCH(bool, backgroundInFlight);
        ManagementFixture fixture;
        IpcManagementPanel panel; connectPanel(panel, fixture);
        auto *read = panel.findChild<QPushButton *>("ipcParametersRead");
        auto *selector = panel.findChild<QComboBox *>("ipcParameterComponent");
        auto *save = panel.findChild<QPushButton *>("ipcParametersSave");
        auto *start = panel.findChild<QPushButton *>("ipcService_tracking_start");
        QTRY_VERIFY(fixture.reads > 0 && read->isEnabled());
        read->click(); QTRY_VERIFY(save->isEnabled());
        fixture.delayNextManagement = true;
        const int before = fixture.reads;
        if (backgroundInFlight) {
            panel.refreshOriginSnapshot();
            QTRY_VERIFY_WITH_TIMEOUT(fixture.delayedManagementSocket, 1000);
            QVERIFY(read->isEnabled());
        }
        read->click();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.delayedManagementSocket, 1000);
        QVERIFY(!selector->isEnabled()); QVERIFY(!read->isEnabled());
        QVERIFY(!save->isEnabled()); QVERIFY(!start->isEnabled());
        read->click(); save->click(); start->click();
        QCOMPARE(fixture.reads, before + 1); QCOMPARE(fixture.writes, 0);
        fixture.releaseManagement();
        QTRY_VERIFY(selector->isEnabled() && read->isEnabled() && save->isEnabled());
        QCOMPARE(selector->currentData().toString(), QString("fusion"));
        QCOMPARE(panel.findChild<QDoubleSpinBox *>("ipcParameter_speed.limit")->value(), fixture.speed);
        QCOMPARE(fixture.writes, 0);
    }
    void liveBackupOnlyKeepsConfigurationAndServicesUnchanged() {
        const auto url = qEnvironmentVariable("TENCO_LIVE_IPC_BACKUP_URL");
        if (url.isEmpty()) QSKIP("Explicit live backup endpoint required; no service switching");
        IpcManagementPanel panel;
        const auto token = qEnvironmentVariable("TENCO_LIVE_IPC_BACKUP_TOKEN");
        QVERIFY(!token.isEmpty());
        panel.setConnectionProvider([url, token] { return IpcManagementConnection{QUrl(url), token, 3500}; });
        panel.resize(1100, 1400); panel.show();
        JsonHttpClient observer;
        observer.configure(QUrl(url), token, 3500);
        auto snapshot = [&observer] {
            QJsonObject value; bool done = false;
            if (!observer.request("observe", "GET", "/management", {}, {}, {}, [&](const JsonHttpResult &r) {
                if (r.ok()) value = r.object; done = true;
            })) return QJsonObject{};
            QElapsedTimer deadline; deadline.start();
            while (!done && deadline.elapsed() < 4500) { QCoreApplication::processEvents(); QTest::qWait(10); }
            return value;
        };
        const auto before = snapshot(); QVERIFY(before.value("parameters").isArray());
        auto *create = panel.findChild<QPushButton *>("ipcBackupCreate");
        QTRY_VERIFY_WITH_TIMEOUT(create->isEnabled(), 8000);
        create->click();
        auto *result = panel.findChild<QLabel *>("ipcManagementResult");
        QTRY_VERIFY_WITH_TIMEOUT(result->text().contains("配置已完整备份并核对"), 20000);
        QTRY_VERIFY_WITH_TIMEOUT(panel.findChild<QLabel *>("ipcBackupState")->text().contains("备份编号"), 8000);
        const auto after = snapshot();
        QCOMPARE(after.value("parameters"), before.value("parameters"));
        QCOMPARE(after.value("services"), before.value("services"));
        QCOMPARE(after.value("origin"), before.value("origin"));
        const auto screenshot = qEnvironmentVariable("TENCO_LIVE_IPC_BACKUP_SCREENSHOT");
        if (!screenshot.isEmpty()) QVERIFY(panel.grab().save(screenshot));
        panel.hide(); observer.cancelAll();
    }
    void lateBackupReadCannotReplacePostSaveBackupList() {
        ManagementFixture fixture; fixture.delayNextBackup = true;
        IpcManagementPanel panel;
        panel.setConnectionProvider([&fixture] { return IpcManagementConnection{fixture.url(), "test-token", 4000}; });
        panel.show();
        QTRY_VERIFY(fixture.delayedBackupSocket);
        auto *read = panel.findChild<QPushButton *>("ipcParametersRead");
        QTRY_VERIFY(read->isEnabled()); read->click();
        auto *save = panel.findChild<QPushButton *>("ipcParametersSave");
        QTRY_VERIFY(save->isEnabled()); save->click();
        QTRY_VERIFY(fixture.operationReads > 0);
        QVERIFY(fixture.delayedBackupSocket);
        ManagementFixture::reply(fixture.delayedBackupSocket, fixture.delayedBackupObject);
        auto *state = panel.findChild<QLabel *>("ipcBackupState");
        QTRY_VERIFY_WITH_TIMEOUT(state->text().contains("test-backup-001"), 6500);
        QCOMPARE(fixture.writes, 1);
    }
    void malformedBackupSummaryCannotClaimNoBackups() {
        ManagementFixture fixture; fixture.malformedBackupReply = true;
        IpcManagementPanel panel; connectPanel(panel, fixture);
        auto *state = panel.findChild<QLabel *>("ipcBackupState");
        QTRY_VERIFY(state->text().contains("摘要格式无效"));
        QVERIFY(!state->text().contains("尚无备份")); QCOMPARE(fixture.writes, 0);
    }
    void backupIsExplicitAndDoesNotChangeServicesOrParameters() {
        ManagementFixture fixture;
        const auto services = fixture.serviceStates;
        IpcManagementPanel panel; connectPanel(panel, fixture);
        auto *create = panel.findChild<QPushButton *>("ipcBackupCreate");
        auto *state = panel.findChild<QLabel *>("ipcBackupState");
        QVERIFY(create && state);
        QTRY_VERIFY(create->isEnabled()); QTRY_VERIFY(fixture.backupReads > 0);
        QCOMPARE(fixture.writes, 0);
        QTRY_VERIFY(state->text().contains("尚无备份"));
        QTest::mouseClick(create, Qt::LeftButton);
        QTRY_COMPARE(fixture.backupWrites, 1);
        QTRY_VERIFY(state->text().contains("test-backup-001"));
        QCOMPARE(fixture.serviceStates, services); QCOMPARE(fixture.revision, QString("1"));
        QCOMPARE(fixture.lastBody.value("operation").toString(), QString("create"));
        QVERIFY(!fixture.lastBody.value("requestId").toString().isEmpty());
        QCOMPARE(fixture.lastAuthorization, QString("Bearer test-token"));
        QVERIFY(state->text().contains("/test/tenco-project/backups/config"));
        QVERIFY(panel.findChild<QLabel *>("ipcParameterTarget")->text().contains("config/fusion/isolated.yaml"));
    }
    void backupLostReplyQueriesSameOperationWithoutResubmitting() {
        ManagementFixture fixture; fixture.dropWriteReply = true;
        IpcManagementPanel panel; connectPanel(panel, fixture);
        auto *create = panel.findChild<QPushButton *>("ipcBackupCreate");
        QTRY_VERIFY(create->isEnabled());
        QTest::mouseClick(create, Qt::LeftButton);
        QTRY_VERIFY(fixture.operationReads > 0);
        QTRY_VERIFY(panel.findChild<QLabel *>("ipcBackupState")->text().contains("test-backup-001"));
        QTest::qWait(100);
        QCOMPARE(fixture.backupWrites, 1); QCOMPARE(fixture.writes, 1);
    }
    void fusionOriginCancelDoesNotWriteOrStart() {
        ManagementFixture fixture; fixture.serviceStates["fusion"] = "inactive";
        const auto saved = fixture.savedOrigin;
        IpcManagementPanel panel; connectPanel(panel, fixture);
        auto *start = panel.findChild<QPushButton *>("ipcService_fusion_start");
        QTRY_VERIFY(start->isEnabled()); QTest::mouseClick(start, Qt::LeftButton);
        auto *dialog = panel.findChild<QDialog *>("ipcFusionOriginDialog");
        QVERIFY(dialog); QVERIFY(dialog->isVisible());
        auto *latitude = dialog->findChild<QDoubleSpinBox *>("ipcOriginLatitude");
        auto *longitude = dialog->findChild<QDoubleSpinBox *>("ipcOriginLongitude");
        QVERIFY(latitude && longitude);
        QVERIFY(qAbs(latitude->value() - 30.222814812345) < 1e-9);
        QVERIFY(qAbs(longitude->value() - 120.022578112345) < 1e-9);
        latitude->setValue(31.123456789); longitude->setValue(121.123456789);
        QVERIFY(dialog->findChild<QLabel *>("ipcOriginChangeNotice")->text().contains("旧地图和旧任务将失效"));
        auto *buttons = dialog->findChild<QDialogButtonBox *>(); QVERIFY(buttons);
        buttons->button(QDialogButtonBox::Cancel)->click();
        QTest::qWait(100);
        QCOMPARE(fixture.writes, 0); QCOMPARE(fixture.savedOrigin, saved);
        QCOMPARE(fixture.serviceStates.value("fusion"), QString("inactive"));
    }
    void fusionOriginUnchangedPreservesAllDigitsAndConfirmsEveryRestart() {
        ManagementFixture fixture; fixture.serviceStates["fusion"] = "inactive";
        const auto saved = fixture.savedOrigin;
        IpcManagementPanel panel; connectPanel(panel, fixture);
        auto *start = panel.findChild<QPushButton *>("ipcService_fusion_start");
        QTRY_VERIFY(start->isEnabled()); QTest::mouseClick(start, Qt::LeftButton);
        const auto screenshot = qEnvironmentVariable("TENCO_ORIGIN_DIALOG_SCREENSHOT");
        if (!screenshot.isEmpty()) {
            auto *dialog = panel.findChild<QDialog *>("ipcFusionOriginDialog"); QVERIFY(dialog);
            QVERIFY(dialog->grab().save(screenshot));
        }
        confirmOrigin(panel);
        QTRY_COMPARE(fixture.writes, 1);
        QCOMPARE(fixture.lastBody.value("origin").toObject(), QJsonObject({{"latitude", saved.value("latitude")}, {"longitude", saved.value("longitude")}, {"altitude", 0}}));
        QCOMPARE(fixture.lastBody.value("expectedOriginRevision").toString(), fixture.originRevision);
        auto *restart = panel.findChild<QPushButton *>("ipcService_fusion_restart");
        QTRY_VERIFY(restart->isEnabled()); QVERIFY(!start->isEnabled());
        QTest::mouseClick(restart, Qt::LeftButton);
        QCOMPARE(fixture.writes, 1); confirmOrigin(panel);
        QTRY_COMPARE(fixture.writes, 2);
        QCOMPARE(fixture.lastBody.value("action").toString(), QString("restart"));
        QCOMPARE(fixture.savedOrigin, saved);
    }
    void fusionOriginModificationUsesCorrectLatitudeAndLongitude() {
        ManagementFixture fixture; fixture.serviceStates["fusion"] = "inactive"; fixture.originConsistent = false;
        IpcManagementPanel panel; connectPanel(panel, fixture);
        auto *start = panel.findChild<QPushButton *>("ipcService_fusion_start");
        QTRY_VERIFY(start->isEnabled()); QTest::mouseClick(start, Qt::LeftButton);
        auto *dialog = panel.findChild<QDialog *>("ipcFusionOriginDialog"); QVERIFY(dialog);
        dialog->findChild<QDoubleSpinBox *>("ipcOriginLatitude")->setValue(-31.012345678);
        dialog->findChild<QDoubleSpinBox *>("ipcOriginLongitude")->setValue(121.987654321);
        confirmOrigin(panel); QTRY_COMPARE(fixture.writes, 1);
        const auto origin = fixture.lastBody.value("origin").toObject();
        QCOMPARE(origin.value("latitude").toDouble(), -31.012345678);
        QCOMPARE(origin.value("longitude").toDouble(), 121.987654321);
        QCOMPARE(origin.value("altitude").toInt(-1), 0);
        QVERIFY(fixture.originConsistent);
    }
    void fusionStartUnlocksRestartAndStopOnlyAfterActualReadback() {
        ManagementFixture fixture;
        fixture.serviceStates["fusion"] = "inactive";
        fixture.operationState = "running";
        IpcManagementPanel panel; connectPanel(panel, fixture);
        auto *start = panel.findChild<QPushButton *>("ipcService_fusion_start");
        auto *restart = panel.findChild<QPushButton *>("ipcService_fusion_restart");
        auto *stop = panel.findChild<QPushButton *>("ipcService_fusion_stop");
        auto *hint = panel.findChild<QLabel *>("ipcServiceCondition");
        QTRY_VERIFY(start->isEnabled()); start->click(); confirmOrigin(panel);
        QTRY_COMPARE(fixture.writes, 1);
        QTRY_VERIFY(fixture.operationReads > 0);
        QVERIFY(!restart->isEnabled()); QVERIFY(!stop->isEnabled());
        QVERIFY(stop->toolTip().contains("维护操作")); QVERIFY(hint->text().contains("维护操作"));
        fixture.delayNextManagement = true;
        fixture.operationState = "applied";
        QTRY_VERIFY_WITH_TIMEOUT(fixture.delayedManagementSocket, 4000);
        QVERIFY(!restart->isEnabled()); QVERIFY(!stop->isEnabled());
        QVERIFY(stop->toolTip().contains("重新读取"));
        fixture.releaseManagement();
        QTRY_VERIFY(restart->isEnabled() && stop->isEnabled());
        QVERIFY(!start->isEnabled());
        restart->click(); confirmOrigin(panel);
        QTRY_COMPARE(fixture.writes, 2);
        QCOMPARE(fixture.lastBody.value("action").toString(), QString("restart"));
        QTRY_VERIFY(stop->isEnabled()); stop->click();
        QTRY_COMPARE(fixture.writes, 3);
        QCOMPARE(fixture.lastBody.value("action").toString(), QString("stop"));
        QTRY_VERIFY(start->isEnabled());
        QVERIFY(!restart->isEnabled()); QVERIFY(!stop->isEnabled());
    }
    void lostFusionOperationRecordRefreshesActualStateWithoutReplay() {
        ManagementFixture fixture;
        fixture.serviceStates["fusion"] = "inactive";
        IpcManagementPanel panel; connectPanel(panel, fixture);
        auto *start = panel.findChild<QPushButton *>("ipcService_fusion_start");
        auto *restart = panel.findChild<QPushButton *>("ipcService_fusion_restart");
        auto *stop = panel.findChild<QPushButton *>("ipcService_fusion_stop");
        auto *result = panel.findChild<QLabel *>("ipcManagementResult");
        QTRY_VERIFY(start->isEnabled());
        fixture.operationStatus = 404;
        fixture.managementBoot = "restarted-management-process";
        fixture.delayNextManagement = true;
        start->click(); confirmOrigin(panel);
        QTRY_COMPARE(fixture.writes, 1);
        QTRY_VERIFY_WITH_TIMEOUT(fixture.delayedManagementSocket, 4000);
        QVERIFY(result->text().contains("结果尚未确认"));
        QVERIFY(result->text().contains("未重复发送"));
        QVERIFY(!restart->isEnabled()); QVERIFY(!stop->isEnabled());
        fixture.releaseManagement();
        QTRY_VERIFY_WITH_TIMEOUT(restart->isEnabled() && stop->isEnabled(), 3000);
        QVERIFY(!start->isEnabled());
        QCOMPARE(fixture.writes, 1);
        QVERIFY(result->text().contains("结果尚未确认"));
        fixture.operationStatus = 200;
        stop->click();
        QTRY_COMPARE(fixture.writes, 2);
        QCOMPARE(fixture.lastBody.value("action").toString(), QString("stop"));
        QTRY_VERIFY(start->isEnabled());
    }
    void fusionParameterApplyUsesSameOriginConfirmation() {
        ManagementFixture fixture; IpcManagementPanel panel; connectPanel(panel, fixture);
        auto *read = panel.findChild<QPushButton *>("ipcParametersRead");
        QTRY_VERIFY(fixture.reads > 0); QTRY_VERIFY(read->isEnabled()); QTest::mouseClick(read, Qt::LeftButton);
        auto *apply = panel.findChild<QPushButton *>("ipcParametersSaveApply");
        QTRY_VERIFY(apply->isEnabled());
        auto *speed = panel.findChild<QDoubleSpinBox *>("ipcParameter_speed.limit"); QVERIFY(speed); speed->setValue(0.3);
        QTest::mouseClick(apply, Qt::LeftButton);
        auto *dialog = panel.findChild<QDialog *>("ipcFusionOriginDialog"); QVERIFY(dialog); dialog->reject();
        QCOMPARE(fixture.writes, 0); QVERIFY(panel.hasUnsavedChanges());
        QTest::qWait(1);
        QTRY_VERIFY(apply->isEnabled()); QTest::mouseClick(apply, Qt::LeftButton); confirmOrigin(panel);
        QTRY_COMPARE(fixture.writes, 1);
        QCOMPARE(fixture.lastPath, QString("/api/v1/management/parameters/fusion"));
        QVERIFY(fixture.lastBody.value("apply").toBool());
        QCOMPARE(fixture.lastBody.value("expectedOriginRevision").toString(), fixture.originRevision);
        QVERIFY(!fixture.lastBody.value("origin").toObject().isEmpty());
    }
    void serviceDependenciesAreVisibleAndCameraIsIndependent() {
        ManagementFixture fixture; fixture.serviceStates["fusion"] = "inactive"; fixture.serviceStates["camera"] = "inactive";
        IpcManagementPanel panel; connectPanel(panel, fixture);
        auto *fusionStart = panel.findChild<QPushButton *>("ipcService_fusion_start");
        auto *trackingStart = panel.findChild<QPushButton *>("ipcService_tracking_start");
        auto *cameraStart = panel.findChild<QPushButton *>("ipcService_camera_start");
        QTRY_VERIFY(fusionStart->isEnabled());
        QVERIFY(!trackingStart->isEnabled()); QVERIFY(cameraStart->isEnabled());
        QVERIFY(panel.findChild<QLabel *>("ipcServiceCondition")->text().contains("先确认本次基准站坐标并启动融合"));
        fixture.serviceStates["fusion"] = "active"; fixture.fusionProfile = "commissioning";
        panel.refreshOriginSnapshot();
        QTRY_VERIFY(fusionStart->isEnabled() == false);
        QTRY_VERIFY(trackingStart->toolTip().contains("实际运行模式")); QVERIFY(!trackingStart->isEnabled());
        fixture.fusionProfile = "isolated"; panel.refreshOriginSnapshot();
        QTRY_VERIFY(trackingStart->isEnabled());
        fixture.originConsistent = false; panel.refreshOriginSnapshot();
        QTRY_VERIFY(trackingStart->toolTip().contains("基准站坐标尚未确认一致")); QVERIFY(!trackingStart->isEnabled());
        fixture.originConsistent = true; panel.refreshOriginSnapshot();
        QTRY_VERIFY(trackingStart->isEnabled());
        fixture.serviceStates["tracking"] = "active"; panel.refreshOriginSnapshot();
        auto *trackingStop = panel.findChild<QPushButton *>("ipcService_tracking_stop");
        QTRY_VERIFY(trackingStop->isEnabled());
        QVERIFY(!panel.findChild<QPushButton *>("ipcService_fusion_restart")->isEnabled());
        QVERIFY(!panel.findChild<QPushButton *>("ipcService_fusion_stop")->isEnabled());
        QVERIFY(panel.findChild<QPushButton *>("ipcService_fusion_restart")->toolTip().contains("先停止导航"));
        QVERIFY(cameraStart->isEnabled()); QCOMPARE(fixture.writes, 0);
    }
    void missingUnifiedOriginDoesNotUseLocalFallback() {
        ManagementFixture fixture; fixture.includeOrigin = false; fixture.serviceStates["fusion"] = "inactive";
        IpcManagementPanel panel; QSignalSpy origin(&panel, &IpcManagementPanel::originSnapshotChanged); connectPanel(panel, fixture);
        QTRY_VERIFY(fixture.reads > 0); QTRY_VERIFY(!origin.isEmpty());
        QVERIFY(origin.last().first().toJsonObject().isEmpty());
        QVERIFY(!panel.findChild<QPushButton *>("ipcService_fusion_start")->isEnabled());
        QTRY_VERIFY(panel.findChild<QLabel *>("ipcOriginState")->text().contains("更新工控机管理服务"));
        QCOMPARE(fixture.writes, 0);
    }
    void originChangedDuringConfirmationIsRejectedWithoutReplay() {
        ManagementFixture fixture; fixture.serviceStates["fusion"] = "inactive";
        IpcManagementPanel panel; connectPanel(panel, fixture);
        auto *start = panel.findChild<QPushButton *>("ipcService_fusion_start");
        QTRY_VERIFY(start->isEnabled()); QTest::mouseClick(start, Qt::LeftButton);
        fixture.originRevision = "origin-config-concurrent";
        confirmOrigin(panel);
        QTRY_VERIFY(panel.findChild<QLabel *>("ipcManagementResult")->text().contains("操作被拒绝"));
        QCOMPARE(fixture.writes, 1); QCOMPARE(fixture.operationReads, 0);
        QCOMPARE(fixture.serviceStates.value("fusion"), QString("inactive"));
    }
    void readBeforeEditingAndExplicitSave() {
        ManagementFixture fixture; IpcManagementPanel panel; connectPanel(panel, fixture);
        QTRY_VERIFY(fixture.reads > 0);
        QCOMPARE(fixture.writes, 0);
        QVERIFY(!panel.findChild<QPushButton *>("ipcParametersSaveApply")->isEnabled());
        auto *read = panel.findChild<QPushButton *>("ipcParametersRead");
        QTRY_VERIFY(read->isEnabled()); QTest::mouseClick(read, Qt::LeftButton);
        QTRY_VERIFY(panel.findChild<QPushButton *>("ipcParametersSaveApply")->isEnabled());
        QCOMPARE(fixture.lastAuthorization, QString("Bearer test-token"));
        const auto screenshot = qEnvironmentVariable("TENCO_MANAGEMENT_PANEL_SCREENSHOT");
        if (!screenshot.isEmpty()) QVERIFY(panel.grab().save(screenshot));
        auto *speed = panel.findChild<QDoubleSpinBox *>("ipcParameter_speed.limit");
        QVERIFY(speed); speed->setValue(0.35);
        QVERIFY(panel.hasUnsavedChanges());
        QTest::mouseClick(panel.findChild<QPushButton *>("ipcParametersSave"), Qt::LeftButton);
        QTRY_COMPARE(fixture.writes, 1);
        QTRY_VERIFY(!panel.hasUnsavedChanges());
        QCOMPARE(fixture.speed, 0.35);
        QCOMPARE(fixture.parameterState, QString("pending"));
        QTRY_VERIFY(panel.findChild<QLabel *>("ipcParameterState")->text().contains("待应用"));
        QTest::mouseClick(panel.findChild<QPushButton *>("ipcParametersApplySaved"), Qt::LeftButton);
        confirmOrigin(panel);
        QTRY_COMPARE(fixture.writes, 2);
        QTRY_VERIFY(panel.findChild<QLabel *>("ipcParameterState")->text().contains("已核验生效"));
    }
    void booleanNavigationParameterLoadsAndSavesAsBoolean_data() {
        QTest::addColumn<bool>("saved");
        QTest::newRow("enable") << false;
        QTest::newRow("disable") << true;
    }
    void booleanNavigationParameterLoadsAndSavesAsBoolean() {
        QFETCH(bool, saved);
        ManagementFixture fixture;
        fixture.parameterState = "pending";
        fixture.customFields = {QJsonObject{{"key", "endpoint_recovery.enabled"},
            {"label", "端点自主回位"}, {"type", "boolean"}, {"value", saved}, {"appliedValue", !saved},
            {"group", "到点与转向完成"}, {"description", "仅在运动证据与返回空间通过核验后回原端点。"}}};
        IpcManagementPanel panel; connectPanel(panel, fixture);
        panel.findChild<QComboBox *>("ipcParameterComponent")->setCurrentIndex(1);
        panel.findChild<QTabWidget *>("ipcMaintenanceTabs")->setCurrentIndex(1);
        auto *read = panel.findChild<QPushButton *>("ipcParametersRead");
        QTRY_VERIFY(read->isEnabled()); read->click();
        QTRY_VERIFY(panel.findChild<QCheckBox *>("ipcParameter_endpoint_recovery.enabled"));
        auto *editor = panel.findChild<QCheckBox *>("ipcParameter_endpoint_recovery.enabled");
        auto *readback = panel.findChild<QLabel *>("ipcParameterReadback_endpoint_recovery.enabled");
        QVERIFY(readback);
        QCOMPARE(editor->isChecked(), saved);
        QVERIFY(!panel.hasUnsavedChanges());
        QVERIFY(readback->text().contains(saved ? "已保存：启用" : "已保存：关闭"));
        QVERIFY(readback->text().contains(saved ? "运行值：关闭" : "运行值：启用"));
        QCOMPARE(fixture.writes, 0);
        editor->setChecked(!saved);
        QVERIFY(panel.hasUnsavedChanges());
        editor->setChecked(saved);
        QVERIFY(!panel.hasUnsavedChanges());
        editor->setChecked(!saved);
        auto *save = panel.findChild<QPushButton *>("ipcParametersSave");
        QVERIFY(save->isEnabled()); save->click();
        QTRY_COMPARE(fixture.writes, 1);
        QCOMPARE(fixture.lastPath, QString("/api/v1/management/parameters/tracking"));
        QCOMPARE(fixture.lastBody.value("expectedRevision"), QJsonValue("1"));
        QVERIFY(!fixture.lastBody.value("apply").toBool());
        const auto values = fixture.lastBody.value("values").toObject();
        QCOMPARE(values.size(), 1);
        QVERIFY(values.value("endpoint_recovery.enabled").isBool());
        QCOMPARE(values.value("endpoint_recovery.enabled").toBool(), !saved);
        QCOMPARE(fixture.serviceStates.value("tracking"), QString("inactive"));
    }
    void malformedBooleanNavigationParameterRejectsWholeForm_data() {
        QTest::addColumn<QJsonValue>("value");
        QTest::newRow("missing") << QJsonValue(QJsonValue::Undefined);
        QTest::newRow("null") << QJsonValue(QJsonValue::Null);
        QTest::newRow("zero") << QJsonValue(0);
        QTest::newRow("one") << QJsonValue(1);
        QTest::newRow("string-true") << QJsonValue("true");
        QTest::newRow("string-false") << QJsonValue("false");
        QTest::newRow("array") << QJsonValue(QJsonArray{});
        QTest::newRow("object") << QJsonValue(QJsonObject{});
    }
    void malformedBooleanNavigationParameterRejectsWholeForm() {
        QFETCH(QJsonValue, value);
        ManagementFixture fixture;
        fixture.customFields = {
            QJsonObject{{"key", "speed.limit"}, {"label", "速度上限"}, {"type", "number"},
                {"minimum", .1}, {"maximum", 1.5}, {"value", .2}},
            QJsonObject{{"key", "endpoint_recovery.enabled"}, {"label", "端点自主回位"},
                {"type", "boolean"}, {"value", value}}};
        IpcManagementPanel panel; connectPanel(panel, fixture);
        panel.findChild<QComboBox *>("ipcParameterComponent")->setCurrentIndex(1);
        auto *read = panel.findChild<QPushButton *>("ipcParametersRead");
        QTRY_VERIFY(read->isEnabled()); read->click();
        QTRY_VERIFY(panel.findChild<QLabel *>("ipcManagementResult")->text().contains("参数定义无效"));
        QVERIFY(!panel.findChild<QWidget *>("ipcParameter_endpoint_recovery.enabled"));
        QVERIFY(!panel.findChild<QWidget *>("ipcParameter_speed.limit"));
        QVERIFY(!panel.findChild<QPushButton *>("ipcParametersSave")->isEnabled());
        QVERIFY(!panel.findChild<QPushButton *>("ipcParametersSaveApply")->isEnabled());
        QVERIFY(!panel.hasUnsavedChanges());
        QCOMPARE(fixture.writes, 0);
    }
    void longParameterFormKeepsActionsVisibleAndNamesRetainedEdits() {
        ManagementFixture fixture;
        for (int i = 0; i < 14; ++i) fixture.customFields.append(QJsonObject{
            {"key", i == 0 ? "speed.limit" : QString("future.parameter_%1").arg(i)},
            {"label", QString("测试参数 %1").arg(i)}, {"type", "number"},
            {"minimum", 0.1}, {"maximum", 1.5}, {"value", 0.2},
            {"description", "参数说明，用于核对长表单滚动时编辑状态与保存操作始终可见。"}});
        IpcManagementPanel panel; connectPanel(panel, fixture);
        panel.resize(950, 580);
        auto *pages = panel.findChild<QTabWidget *>("ipcMaintenanceTabs");
        QVERIFY(pages); pages->setCurrentIndex(1);
        auto *read = panel.findChild<QPushButton *>("ipcParametersRead");
        QTRY_VERIFY(fixture.reads > 0 && read->isEnabled()); read->click();
        QTRY_VERIFY(panel.findChild<QDoubleSpinBox *>("ipcParameter_speed.limit"));
        auto *editor = panel.findChild<QDoubleSpinBox *>("ipcParameter_speed.limit");
        auto *save = panel.findChild<QPushButton *>("ipcParametersSave");
        auto *state = panel.findChild<QLabel *>("ipcParameterEditState");
        auto *scroll = qobject_cast<QScrollArea *>(pages->currentWidget());
        QVERIFY(scroll && state && save); QTRY_VERIFY(save->isVisible());
        QTRY_VERIFY(scroll->verticalScrollBar()->maximum() > 0);
        const auto actionPosition = save->mapTo(&panel, QPoint());
        scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
        QCoreApplication::processEvents();
        QCOMPARE(save->mapTo(&panel, QPoint()), actionPosition);
        QVERIFY(panel.rect().contains(QRect(actionPosition, save->size())));
        editor->setValue(0.4);
        QVERIFY(state->text().contains("正在编辑：融合参数"));
        QVERIFY(state->text().contains("有未保存修改"));
        auto *selector = panel.findChild<QComboBox *>("ipcParameterComponent");
        selector->setCurrentIndex(1);
        QVERIFY(state->text().contains("保留的编辑：融合参数"));
        QVERIFY(!save->isEnabled()); QVERIFY(!editor->isEnabled());
        QVERIFY(save->toolTip().contains("另一组件"));
        QCOMPARE(editor->value(), 0.4);
        selector->setCurrentIndex(0);
        QVERIFY(save->isEnabled()); QVERIFY(editor->isEnabled());
        QVERIFY(state->text().contains("正在编辑：融合参数"));
        QVERIFY(panel.hasUnsavedChanges());
        QCOMPARE(editor->value(), 0.4); QCOMPARE(fixture.writes, 0);
        pages->setCurrentIndex(0); QVERIFY(!save->isVisible());
    }
    void dynamicNavigationSchemaShowsGroupsImpactAndAppliedReadback() {
        ManagementFixture fixture;
        fixture.parameterState = "pending";
        fixture.revision = "2";
        fixture.customFields = {
            QJsonObject{{"key", "pure_pursuit.lookahead_min_m"}, {"label", "最小前视距离"}, {"type", "number"},
                {"minimum", 0.05}, {"maximum", 5}, {"value", 0.35}, {"appliedValue", 0.3}, {"unit", "m"},
                {"group", "路径跟踪与前视"}, {"description", "减小后纠偏更快，过小可能摆动；增大后行走更平滑。"}},
            QJsonObject{{"key", "control.angular_accel_radps2"}, {"label", "角加速度"}, {"type", "number"},
                {"minimum", 0.05}, {"maximum", 3}, {"value", 0.2}, {"appliedValue", 0.2}, {"unit", "rad/s²"},
                {"group", "速度与加减速"}, {"description", "控制转向命令上升速度，不改变转向速度上限。"}},
            QJsonObject{{"key", "pure_pursuit.lookahead_time_s"}, {"label", "前视时间"}, {"type", "number"},
                {"minimum", 0}, {"maximum", 5}, {"value", 1.5}, {"appliedValue", 1.5}, {"unit", "s"},
                {"group", "路径跟踪与前视"}, {"description", "按当前速度增加前视距离，并受最小和最大前视距离限制。"}},
            QJsonObject{{"key", "future.gain"}, {"label", "新增导航参数"}, {"type", "number"},
                {"minimum", 0}, {"maximum", 5}, {"value", 1.2}, {"unit", ""},
                {"group", "新增参数分组"}, {"description", "新增字段说明来自工控机。"}}
        };
        IpcManagementPanel panel; connectPanel(panel, fixture);
        panel.findChild<QComboBox *>("ipcParameterComponent")->setCurrentIndex(1);
        panel.findChild<QTabWidget *>("ipcMaintenanceTabs")->setCurrentIndex(1);
        auto *read = panel.findChild<QPushButton *>("ipcParametersRead");
        QTRY_VERIFY(read->isEnabled()); read->click();
        QTRY_VERIFY(panel.findChild<QDoubleSpinBox *>("ipcParameter_future.gain"));
        QCOMPARE(panel.findChild<QDoubleSpinBox *>("ipcParameter_pure_pursuit.lookahead_time_s")->value(), 1.5);
        QCOMPARE(panel.findChild<QDoubleSpinBox *>("ipcParameter_control.angular_accel_radps2")->value(), 0.2);
        QStringList groupNames;
        for (auto *label : panel.findChildren<QLabel *>())
            if (label->objectName().startsWith("ipcParameterGroup_")) groupNames.append(label->text());
        QCOMPARE(groupNames, QStringList({"路径跟踪与前视", "速度与加减速", "新增参数分组"}));
        QVERIFY(panel.findChild<QLabel *>("ipcParameterDescription_pure_pursuit.lookahead_min_m")->text().contains("过小可能摆动"));
        const auto applied = panel.findChild<QLabel *>("ipcParameterReadback_pure_pursuit.lookahead_min_m")->text();
        QVERIFY(applied.contains("已保存：0.35 m")); QVERIFY(applied.contains("运行值：0.3 m"));
        QVERIFY(panel.findChild<QLabel *>("ipcParameterReadback_future.gain")->text().contains("尚未核验"));
        const auto screenshot = qEnvironmentVariable("TENCO_MANAGEMENT_SCHEMA_SCREENSHOT");
        if (!screenshot.isEmpty()) QVERIFY(panel.grab().save(screenshot));
        QCOMPARE(fixture.writes, 0);
    }
    void readbackRefreshPreservesEditsAndNeverSubstitutesSavedForUnverifiedRuntime() {
        ManagementFixture fixture;
        fixture.parameterState = "pending";
        fixture.customFields = {QJsonObject{{"key", "speed.limit"}, {"label", "速度上限"}, {"type", "number"},
            {"minimum", 0.1}, {"maximum", 1.5}, {"value", 0.35}, {"appliedValue", 0.2}, {"unit", "m/s"}}};
        IpcManagementPanel panel; connectPanel(panel, fixture);
        auto *read = panel.findChild<QPushButton *>("ipcParametersRead");
        QTRY_VERIFY(read->isEnabled()); read->click();
        QTRY_VERIFY(panel.findChild<QDoubleSpinBox *>("ipcParameter_speed.limit"));
        auto *editor = panel.findChild<QDoubleSpinBox *>("ipcParameter_speed.limit");
        auto *readback = panel.findChild<QLabel *>("ipcParameterReadback_speed.limit");
        QVERIFY(readback->text().contains("运行值：0.2 m/s"));
        editor->setValue(0.45);
        fixture.parameterState = "unverified";
        panel.refreshOriginSnapshot();
        QTRY_VERIFY(readback->text().contains("运行值：尚未核验"));
        QCOMPARE(editor->value(), 0.45); QVERIFY(panel.hasUnsavedChanges());
        QVERIFY(readback->text().contains("已保存：0.35 m/s"));
        fixture.parameterState = "pending";
        fixture.runtimeRevisionAvailable = false;
        const auto reads = fixture.reads;
        panel.refreshOriginSnapshot();
        QTRY_VERIFY(fixture.reads > reads);
        QTRY_VERIFY(panel.findChild<QLabel *>("ipcParameterState")->text().contains("待应用"));
        QVERIFY(readback->text().contains("运行值：尚未核验"));
        fixture.parameterState = "applied";
        fixture.runtimeRevisionAvailable = true;
        auto field = fixture.customFields.first().toObject(); field.insert("appliedValue", 0.35);
        fixture.customFields[0] = field;
        panel.refreshOriginSnapshot();
        QTRY_VERIFY(readback->text().contains("运行值：0.35 m/s"));
        QCOMPARE(editor->value(), 0.45); QCOMPARE(fixture.writes, 0);
    }
    void offlineReadbackRetainsSavedValuesAndEdits_data() {
        QTest::addColumn<bool>("invalidConnection");
        QTest::newRow("request-failed") << false;
        QTest::newRow("invalid-connection") << true;
    }
    void offlineReadbackRetainsSavedValuesAndEdits() {
        QFETCH(bool, invalidConnection);
        ManagementFixture fixture;
        fixture.parameterState = "pending"; fixture.revision = "2";
        fixture.customFields = {QJsonObject{{"key", "speed.limit"}, {"label", "速度上限"}, {"type", "number"},
            {"minimum", 0.1}, {"maximum", 1.5}, {"value", 0.35}, {"appliedValue", 0.2}, {"unit", "m/s"}}};
        IpcManagementPanel panel; connectPanel(panel, fixture);
        bool invalid = false;
        panel.setConnectionProvider([&] { return IpcManagementConnection{invalid ? QUrl() : fixture.url(), "test-token", 500}; });
        auto *read = panel.findChild<QPushButton *>("ipcParametersRead");
        QTRY_VERIFY(read->isEnabled()); read->click();
        QTRY_VERIFY(panel.findChild<QDoubleSpinBox *>("ipcParameter_speed.limit"));
        auto *editor = panel.findChild<QDoubleSpinBox *>("ipcParameter_speed.limit");
        auto *readback = panel.findChild<QLabel *>("ipcParameterReadback_speed.limit");
        QVERIFY(readback->text().contains("运行值：0.2 m/s"));
        editor->setValue(0.45);
        const auto port = fixture.serverPort();
        if (invalidConnection) invalid = true;
        else fixture.close();
        panel.refreshOriginSnapshot();
        QTRY_VERIFY(readback->text().contains("尚未核验（管理服务离线）"));
        QVERIFY(readback->text().contains("已保存：0.35 m/s"));
        QCOMPARE(editor->value(), 0.45); QVERIFY(panel.hasUnsavedChanges());
        QVERIFY(!panel.findChild<QPushButton *>("ipcParametersSave")->isEnabled());
        QVERIFY(panel.findChild<QPushButton *>("ipcParametersSave")->toolTip().contains("离线"));
        QVERIFY(panel.findChild<QLabel *>("ipcParameterEditState")->text().contains("有未保存修改"));
        if (invalidConnection) invalid = false;
        else QVERIFY(fixture.listen(QHostAddress::LocalHost, port));
        fixture.parameterState = "applied";
        auto field = fixture.customFields.first().toObject(); field.insert("appliedValue", 0.35);
        fixture.customFields[0] = field;
        panel.refreshOriginSnapshot();
        QTRY_VERIFY(readback->text().contains("运行值：0.35 m/s"));
        QCOMPARE(editor->value(), 0.45); QVERIFY(panel.hasUnsavedChanges());
        QCOMPARE(fixture.writes, 0);
    }
    void lostWriteResponseQueriesOperationWithoutReplay() {
        ManagementFixture fixture; fixture.dropWriteReply = true;
        IpcManagementPanel panel; connectPanel(panel, fixture);
        auto *stop = panel.findChild<QPushButton *>("ipcService_camera_stop");
        QTRY_VERIFY(stop->isEnabled()); QTest::mouseClick(stop, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(fixture.operationReads > 0, 4000);
        QCOMPARE(fixture.writes, 1);
        QCOMPARE(fixture.lastPath, QString("/api/v1/management/services/camera"));
        QCOMPARE(fixture.lastBody.value("action").toString(), QString("stop"));
        QTRY_VERIFY(panel.findChild<QPushButton *>("ipcService_camera_start")->isEnabled());
    }
    void explicitSaveCanPersistUnchangedReadback() {
        ManagementFixture fixture; IpcManagementPanel panel; connectPanel(panel, fixture);
        auto *read = panel.findChild<QPushButton *>("ipcParametersRead");
        auto *save = panel.findChild<QPushButton *>("ipcParametersSave");
        QTRY_VERIFY(fixture.reads > 0); QTRY_VERIFY(read->isEnabled());
        QTest::mouseClick(read, Qt::LeftButton); QTRY_VERIFY(save->isEnabled());
        QVERIFY(!panel.hasUnsavedChanges());
        QTest::mouseClick(save, Qt::LeftButton); QTRY_COMPARE(fixture.writes, 1);
        QCOMPARE(fixture.lastBody.value("values").toObject(), QJsonObject({{"speed.limit", 0.2}}));
        QVERIFY(!fixture.lastBody.value("apply").toBool());
        QCOMPARE(fixture.speed, 0.2);
        QTRY_VERIFY(panel.findChild<QPushButton *>("ipcParametersApplySaved")->isEnabled());
    }
    void foreignRevisionPreservesEditsAndRequiresRead() {
        ManagementFixture fixture; IpcManagementPanel panel; connectPanel(panel, fixture);
        auto *read = panel.findChild<QPushButton *>("ipcParametersRead");
        QTRY_VERIFY(fixture.reads > 0); QTRY_VERIFY(read->isEnabled()); QTest::mouseClick(read, Qt::LeftButton);
        QTRY_VERIFY(panel.findChild<QDoubleSpinBox *>("ipcParameter_speed.limit"));
        auto *speed = panel.findChild<QDoubleSpinBox *>("ipcParameter_speed.limit");
        speed->setValue(0.4); fixture.revision = "22";
        QTRY_VERIFY_WITH_TIMEOUT(panel.findChild<QLabel *>("ipcParameterState")->text().contains("重新读取"), 4000);
        QCOMPARE(speed->value(), 0.4);
        QVERIFY(!panel.findChild<QPushButton *>("ipcParametersSaveApply")->isEnabled());
        QCOMPARE(fixture.writes, 0);
    }
    void rejectionNeverClaimsApplied() {
        ManagementFixture fixture; fixture.reject = true;
        fixture.serviceStates["tracking"] = "active";
        IpcManagementPanel panel; connectPanel(panel, fixture);
        auto *restart = panel.findChild<QPushButton *>("ipcService_tracking_restart");
        QTRY_VERIFY(restart->isEnabled()); QTest::mouseClick(restart, Qt::LeftButton);
        QTRY_VERIFY(panel.findChild<QLabel *>("ipcManagementResult")->text().contains("操作被拒绝"));
        QCOMPARE(fixture.writes, 1); QCOMPARE(fixture.operationReads, 0);
        panel.hide(); const int reads = fixture.reads;
        QTest::qWait(2200); QCOMPARE(fixture.reads, reads);
    }
    void explicitSessionReleaseHasTaskGateAndCanReacquire() {
        ManagementFixture fixture;
        TrackingClient tracking; tracking.configure(fixture.url(), {}, 500); tracking.start();
        QString message;
        QVERIFY(!tracking.releaseSessionForMaintenance(&message));
        QTRY_VERIFY(tracking.fresh()); tracking.acquireSession(); QTRY_VERIFY(tracking.hasSession());
        QTRY_VERIFY(fixture.heartbeats > 0);
        fixture.navigationState = "Paused";
        QTRY_COMPARE(tracking.snapshot().state, QString("Paused"));
        QVERIFY(!tracking.releaseSessionForMaintenance(&message));
        QVERIFY(message.contains("暂停任务不能释放"));
        QVERIFY(tracking.hasSession());
        fixture.navigationState = "Tracking";
        QTRY_COMPARE(tracking.snapshot().state, QString("Tracking"));
        QVERIFY(!tracking.releaseSessionForMaintenance(&message));
        fixture.navigationState = "Ready";
        QTRY_COMPARE(tracking.snapshot().state, QString("Ready"));
        QVERIFY(tracking.releaseSessionForMaintenance(&message));
        QVERIFY(!tracking.hasSession());
        QTest::qWait(100);
        const int heartbeats = fixture.heartbeats;
        QTest::qWait(1100);
        QCOMPARE(fixture.heartbeats, heartbeats);
        QVERIFY(tracking.fresh());
        tracking.acquireSession(); QTRY_VERIFY(tracking.hasSession());
        QTRY_VERIFY(fixture.heartbeats > heartbeats);
        const int reacquired = fixture.heartbeats;
        QTRY_VERIFY(fixture.heartbeats > reacquired);
        QCOMPARE(fixture.sessionRequests, 2);
        QCOMPARE(fixture.writes, 0); // Release never sends pause/abort/start.
    }
    void liveIpcPanelServicesAndUnchangedParameters() {
        const QString address = qEnvironmentVariable("TENCO_LIVE_MANAGEMENT_URL");
        const QString token = qEnvironmentVariable("TENCO_LIVE_API_TOKEN");
        if (address.isEmpty() || token.isEmpty())
            QSKIP("Set TENCO_LIVE_MANAGEMENT_URL and TENCO_LIVE_API_TOKEN for an explicitly scheduled isolated IPC run");
        const QUrl endpoint(address);
        QVERIFY(endpoint.isValid() && !endpoint.host().isEmpty() && endpoint.userInfo().isEmpty());
        QVERIFY(endpoint.scheme() == "http" || endpoint.scheme() == "https");

        // This observer only checks returned schema and persistent values. All
        // service/configuration writes below go through actual panel buttons.
        JsonHttpClient observer;
        observer.configure(endpoint, token, 5000);
        QJsonObject snapshot;
        QTimer observeTimer;
        auto observe = [&] {
            if (observer.busy("read")) return;
            observer.request("read", "GET", "/management", {}, {}, {}, [&](const JsonHttpResult &result) {
                if (result.ok()) snapshot = result.object;
            }, 128 * 1024);
        };
        connect(&observeTimer, &QTimer::timeout, this, observe);
        observeTimer.start(1000); observe();
        QTRY_VERIFY_WITH_TIMEOUT(!snapshot.isEmpty(), 12000);
        QCOMPARE(snapshot.value("apiVersion").toInt(), 1);
        QCOMPARE(snapshot.value("profile").toString(), QString("isolated"));
        QVERIFY(!snapshot.value("bootId").toString().isEmpty());
        auto service = [&](const QString &component) {
            for (const auto &value : snapshot.value("services").toArray())
                if (value.toObject().value("id") == component) return value.toObject();
            return QJsonObject{};
        };
        auto parameters = [&](const QString &component) {
            for (const auto &value : snapshot.value("parameters").toArray())
                if (value.toObject().value("component") == component) return value.toObject();
            return QJsonObject{};
        };
        auto values = [](const QJsonObject &component) {
            QJsonObject result;
            for (const auto &value : component.value("fields").toArray()) {
                const auto field = value.toObject();
                result.insert(field.value("key").toString(), field.value("value"));
            }
            return result;
        };
        // Refuse to take over any pre-existing navigation/fusion process.
        QCOMPARE(service("fusion").value("activeState").toString(), QString("inactive"));
        QCOMPARE(service("tracking").value("activeState").toString(), QString("inactive"));
        const QString cameraState = service("camera").value("activeState").toString();
        QHash<QString, QJsonObject> original;
        QHash<QString, QString> originalRevision;
        for (const auto &component : {QString("fusion"), QString("tracking")}) {
            const auto configuration = parameters(component);
            QVERIFY(!configuration.value("fields").toArray().isEmpty());
            QVERIFY(configuration.value("revision").isString());
            original.insert(component, values(configuration));
            originalRevision.insert(component, configuration.value("revision").toString());
        }

        IpcManagementPanel panel;
        SupportPageStyle::apply(&panel);
        panel.setConnectionProvider([&] { return IpcManagementConnection{endpoint, token, 5000}; });
        panel.resize(1050, 1100); panel.show();
        auto *selector = panel.findChild<QComboBox *>("ipcParameterComponent");
        auto *read = panel.findChild<QPushButton *>("ipcParametersRead");
        auto *save = panel.findChild<QPushButton *>("ipcParametersSave");
        auto *applySaved = panel.findChild<QPushButton *>("ipcParametersApplySaved");
        auto *state = panel.findChild<QLabel *>("ipcParameterState");
        auto *result = panel.findChild<QLabel *>("ipcManagementResult");
        QVERIFY(selector && read && save && applySaved && state && result);
        QTimer dialogs;
        connect(&dialogs, &QTimer::timeout, this, [&] {
            for (auto *widget : QApplication::topLevelWidgets()) {
                auto *box = qobject_cast<QMessageBox *>(widget);
                if (box && box->isVisible() && box->parentWidget() == &panel) {
                    if (auto *yes = box->button(QMessageBox::Yes)) yes->click();
                    else box->accept();
                }
                auto *dialog = qobject_cast<QDialog *>(widget);
                if (dialog && dialog->objectName() == "ipcFusionOriginDialog" && dialog->isVisible() && dialog->parentWidget() == &panel)
                    dialog->findChild<QPushButton *>("ipcOriginConfirm")->click();
            }
        });
        dialogs.start(50);
        const QString screenshotDirectory = qEnvironmentVariable("TENCO_LIVE_MANAGEMENT_SCREENSHOTS");
        if (!screenshotDirectory.isEmpty()) QVERIFY(QDir().mkpath(screenshotDirectory));
        for (const auto &component : {QString("fusion"), QString("tracking")}) {
            if (component == "tracking") {
                // Navigation depends on a running fusion process, independently
                // of whether an isolated fixture currently has pose input.
                auto *fusionStart = panel.findChild<QPushButton *>("ipcService_fusion_start");
                QTRY_VERIFY_WITH_TIMEOUT(fusionStart->isEnabled(), 12000);
                QTest::mouseClick(fusionStart, Qt::LeftButton);
                QTRY_COMPARE_WITH_TIMEOUT(service("fusion").value("activeState").toString(), QString("active"), 70000);
            }
            QTRY_VERIFY_WITH_TIMEOUT(selector->isEnabled(), 12000);
            selector->setCurrentIndex(selector->findData(component));
            QTRY_VERIFY_WITH_TIMEOUT(read->isEnabled(), 12000);
            QTest::mouseClick(read, Qt::LeftButton);
            QTRY_VERIFY_WITH_TIMEOUT(save->isEnabled(), 12000);
            QVERIFY(!panel.hasUnsavedChanges());
            const auto fields = parameters(component).value("fields").toArray();
            for (const auto &value : fields) {
                const auto field = value.toObject();
                const QString name = "ipcParameter_" + field.value("key").toString();
                const QString type = field.value("type").toString();
                if (type == "string") {
                    auto *editor = panel.findChild<QLineEdit *>(name); QVERIFY(editor);
                    QCOMPARE(editor->text(), field.value("value").toString());
                } else if (type == "integer") {
                    auto *editor = panel.findChild<QSpinBox *>(name); QVERIFY(editor);
                    QCOMPARE(editor->value(), field.value("value").toInt());
                } else {
                    QCOMPARE(type, QString("number"));
                    auto *editor = panel.findChild<QDoubleSpinBox *>(name); QVERIFY(editor);
                    QVERIFY(qAbs(editor->value() - field.value("value").toDouble()) <= 1e-9);
                }
            }
            if (!screenshotDirectory.isEmpty()) QVERIFY(panel.grab().save(QDir(screenshotDirectory).filePath(component + "-read.png")));

            // No field setter is used: persist the exact, unchanged values.
            QTest::mouseClick(save, Qt::LeftButton);
            QVERIFY(!save->isEnabled());
            QTRY_VERIFY_WITH_TIMEOUT(applySaved->isEnabled(), 20000);
            QTRY_COMPARE_WITH_TIMEOUT(parameters(component).value("state").toString(), QString("pending"), 12000);
            QCOMPARE(values(parameters(component)), original.value(component));
            QCOMPARE(parameters(component).value("revision").toString(), originalRevision.value(component));
            QTest::mouseClick(applySaved, Qt::LeftButton);
            QVERIFY(!applySaved->isEnabled());
            QTRY_VERIFY_WITH_TIMEOUT(state->text().contains("已核验生效"), 70000);
            QTRY_COMPARE_WITH_TIMEOUT(parameters(component).value("state").toString(), QString("applied"), 12000);
            QCOMPARE(parameters(component).value("appliedRevision").toString(), originalRevision.value(component));
            QCOMPARE(values(parameters(component)), original.value(component));
            if (!screenshotDirectory.isEmpty()) QVERIFY(panel.grab().save(QDir(screenshotDirectory).filePath(component + "-applied.png")));

            auto *restart = panel.findChild<QPushButton *>("ipcService_" + component + "_restart");
            auto *stop = panel.findChild<QPushButton *>("ipcService_" + component + "_stop");
            auto *start = panel.findChild<QPushButton *>("ipcService_" + component + "_start");
            auto *serviceState = panel.findChild<QLabel *>("ipcServiceState_" + component);
            QVERIFY(restart && stop && start && serviceState);
            QTRY_VERIFY_WITH_TIMEOUT(restart->isEnabled(), 12000);
            QTest::mouseClick(restart, Qt::LeftButton);
            QVERIFY(!restart->isEnabled());
            QTRY_VERIFY_WITH_TIMEOUT(result->text().contains("实际参数已读回确认"), 70000);
            QTRY_VERIFY_WITH_TIMEOUT(stop->isEnabled(), 12000);
            QTest::mouseClick(stop, Qt::LeftButton);
            QTRY_VERIFY_WITH_TIMEOUT(result->text().contains("服务已停止"), 40000);
            QTRY_VERIFY_WITH_TIMEOUT(start->isEnabled() && serviceState->text().contains("已停止"), 12000);
            QTest::mouseClick(start, Qt::LeftButton);
            QTRY_VERIFY_WITH_TIMEOUT(result->text().contains("实际参数已读回确认"), 70000);
            QTRY_VERIFY_WITH_TIMEOUT(stop->isEnabled(), 12000);
            QTest::mouseClick(stop, Qt::LeftButton);
            QTRY_VERIFY_WITH_TIMEOUT(result->text().contains("服务已停止"), 40000);
            QTRY_COMPARE_WITH_TIMEOUT(service(component).value("activeState").toString(), QString("inactive"), 12000);
            QTRY_VERIFY_WITH_TIMEOUT(start->isEnabled(), 12000);
            QCOMPARE(values(parameters(component)), original.value(component));
            QCOMPARE(parameters(component).value("revision").toString(), originalRevision.value(component));
            if (!screenshotDirectory.isEmpty()) QVERIFY(panel.grab().save(QDir(screenshotDirectory).filePath(component + "-stopped.png")));
        }
        auto *fusionStop = panel.findChild<QPushButton *>("ipcService_fusion_stop");
        QTRY_VERIFY_WITH_TIMEOUT(fusionStop->isEnabled(), 12000);
        QTest::mouseClick(fusionStop, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(service("fusion").value("activeState").toString(), QString("inactive"), 40000);
        QCOMPARE(service("camera").value("activeState").toString(), cameraState);
        QCOMPARE(service("fusion").value("activeState").toString(), QString("inactive"));
        QCOMPARE(service("tracking").value("activeState").toString(), QString("inactive"));
        dialogs.stop(); observeTimer.stop(); panel.hide(); observer.cancelAll();
        qInfo("真实 Qt 工控机维护页验证完成：参数原值保存与应用核验、融合/导航启停重启；相机未切换，参数数值与版本保持一致。");
    }
};
QTEST_MAIN(IpcManagementTest)
#include "test_ipc_management.moc"
