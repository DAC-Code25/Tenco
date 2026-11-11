#include "../map.h"
#include "../mapgraphicsview.h"
#include "../mapframeadapter.h"
#include "../controlsessioncoordinator.h"

#include "ui_mainwindow.h"

#include <QAction>
#include <QComboBox>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QJsonDocument>
#include <QJsonArray>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <QtTest>

#include <cmath>
#include <limits>
#include <memory>
#include <utility>

class MapMonitorFixture : public QTcpServer
{
public:
    QString state = "Tracking";
    quint64 sequence = 0;
    int writes = 0;
    MapMonitorFixture()
    {
        listen(QHostAddress::LocalHost);
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto *socket = nextPendingConnection();
                auto bytes = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, bytes] {
                    bytes->append(socket->readAll());
                    if (!bytes->contains("\r\n\r\n") || socket->property("handled").toBool()) return;
                    socket->setProperty("handled", true);
                    if (bytes->startsWith("POST ")) ++writes;
                    const auto path = bytes->split(' ').value(1);
                    QJsonObject response{{"apiVersion", 1}, {"bootId", "ui-test"},
                                         {"ready", true}, {"events", QJsonArray{}}};
                    if (path.startsWith("/api/v1/status")) {
                        response["seq"] = QString::number(++sequence);
                        response["stateVersion"] = QString::number(sequence);
                        response["state"] = state;
                        response["taskId"] = "running-task";
                        response["taskRevision"] = 1;
                        response["executionId"] = "running-execution";
                        response["measuredTwist"] = QJsonObject{{"v", 0}, {"omega", 0}, {"valid", true}};
                        response["chassisConnected"] = true;
                        response["measuredStopped"] = true;
                        for (const auto *field : {"stepProgressMeters", "stepLengthMeters", "lateralErrorMeters",
                             "headingErrorRad", "remainingAngleRad", "remainingWaitMs"}) response[field] = 0;
                    }
                    const auto data = QJsonDocument(response).toJson(QJsonDocument::Compact);
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
                                  + QByteArray::number(data.size()) + "\r\nConnection: close\r\n\r\n" + data);
                    socket->disconnectFromHost();
                });
            }
        });
    }
    QUrl url() const { return QUrl(QString("http://127.0.0.1:%1/api/v1").arg(serverPort())); }
};

// Real clients and actual button clicks use only this loopback service. In
// particular, task readback and explicit motion writes are counted separately.
class MapTaskReviewFixture : public QTcpServer
{
public:
    QString state = "Paused", execution = "running-execution", waitingEvent, step = "wait";
    quint64 sequence = 0;
    int taskReads = 0, sessions = 0;
    QStringList controls;
    QJsonObject plan;
    QString lastRequest;
    bool holdNextTaskRead = false;
    QPointer<QTcpSocket> heldTaskRead;

    MapTaskReviewFixture()
    {
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
                    int length = 0;
                    for (const auto &line : bytes->left(end).split('\n'))
                        if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toInt();
                    if (bytes->size() < end + 4 + length) return;
                    socket->setProperty("handled", true);
                    const auto path = bytes->left(end).split(' ').value(1);
                    const auto body = QJsonDocument::fromJson(bytes->mid(end + 4, length)).object();
                    QJsonObject response{{"apiVersion", 1}, {"bootId", "ui-test"},
                                         {"ready", true}, {"events", QJsonArray{}}};
                    int code = 200;
                    if (path.startsWith("/api/v1/status")) {
                        response["seq"] = QString::number(++sequence);
                        response["stateVersion"] = "1";
                        response["state"] = state;
                        response["taskId"] = "running-task";
                        response["taskRevision"] = 1;
                        response["executionId"] = execution;
                        response["stepId"] = step;
                        response["waitingEventId"] = waitingEvent;
                        response["measuredTwist"] = QJsonObject{{"v", 0}, {"omega", 0}, {"valid", true}};
                        response["chassisConnected"] = true;
                        response["measuredStopped"] = true;
                        for (const auto *field : {"stepProgressMeters", "stepLengthMeters", "lateralErrorMeters",
                             "headingErrorRad", "remainingAngleRad", "remainingWaitMs"}) response[field] = 0;
                    } else if (path.endsWith("/configuration")) {
                        response["profile_revision"] = "profile";
                        response["calibration"] = QJsonObject{{"calibrationId", "cal"}};
                        response["origin"] = QJsonObject{{"active", QJsonObject{{"originRevision", "origin"}}}};
                        response["navigation_defaults"] = QJsonObject{{"rotation_policy", "vehicle_footprint"}};
                    } else if (path.endsWith("/sessions")) {
                        ++sessions;
                        response["sessionId"] = "session"; response["sessionToken"] = "token";
                        response["expiresInMs"] = 10000;
                    } else if (path.endsWith("/heartbeat")) {
                        response["sessionId"] = "session"; response["stateVersion"] = "1";
                        response["motionPermit"] = "permit"; response["permitValidForMs"] = 3000;
                        response["expiresInMs"] = 10000;
                    } else if (path.startsWith("/api/v1/tasks/running-task?")) {
                        ++taskReads;
                        if (holdNextTaskRead) {
                            holdNextTaskRead = false;
                            heldTaskRead = socket;
                            return;
                        }
                        response["taskId"] = "running-task"; response["revision"] = 1;
                        response["planHash"] = "fnv1a64:0123456789abcdef"; response["plan"] = plan;
                    } else if (path.endsWith("/control")) {
                        controls.append(body["cmd"].toString());
                        lastRequest = body["requestId"].toString();
                        response = {{"commandId", "motion"}}; code = 202;
                    } else if (path.startsWith("/api/v1/commands")) {
                        response = {{"commandId", "motion"}, {"requestId", lastRequest}, {"state", "applied"}};
                        if (lastRequest.isEmpty()) code = 404;
                    }
                    const auto data = QJsonDocument(response).toJson(QJsonDocument::Compact);
                    socket->write("HTTP/1.1 " + QByteArray::number(code)
                        + " Result\r\nContent-Type: application/json\r\nContent-Length: "
                        + QByteArray::number(data.size()) + "\r\nConnection: close\r\n\r\n" + data);
                    socket->disconnectFromHost();
                });
            }
        });
    }
    QUrl url() const { return QUrl(QString("http://127.0.0.1:%1/api/v1").arg(serverPort())); }
    void releaseHeldTaskRead()
    {
        if (!heldTaskRead) return;
        const auto data = QJsonDocument(QJsonObject{{"apiVersion", 1}, {"taskId", "running-task"},
            {"revision", 1}, {"planHash", "fnv1a64:0123456789abcdef"}, {"plan", plan}})
                .toJson(QJsonDocument::Compact);
        heldTaskRead->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
            + QByteArray::number(data.size()) + "\r\nConnection: close\r\n\r\n" + data);
        heldTaskRead->disconnectFromHost();
        heldTaskRead.clear();
    }
};

class NonNativeFileDialogs
{
    const bool previous = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
public:
    NonNativeFileDialogs() { QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true); }
    ~NonNativeFileDialogs() { QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, previous); }
};

class MapRouteIntegrationTest : public QObject
{
    Q_OBJECT

private:
    template<typename T>
    T *requireChild(QWidget *root, const char *name)
    {
        T *child = root->findChild<T *>(QString::fromLatin1(name));
        if (!child) {
            QTest::qFail(name, __FILE__, __LINE__);
            return nullptr;
        }
        return child;
    }

    void pumpEvents()
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    }

    void setComboToPointId(QComboBox *combo, int pointId)
    {
        QVERIFY(combo != nullptr);
        const int index = combo->findData(pointId);
        QVERIFY2(index >= 0, "point id not found in combo");
        combo->setCurrentIndex(index);
        pumpEvents();
    }

    void addPoint(QWidget *root, double x, double y, double theta = 0.0)
    {
        auto *xSpin = requireChild<QDoubleSpinBox>(root, "mapPointXSpin");
        auto *ySpin = requireChild<QDoubleSpinBox>(root, "mapPointYSpin");
        auto *thetaSpin = requireChild<QDoubleSpinBox>(root, "mapPointThetaSpin");
        auto *button = requireChild<QPushButton>(root, "mapAddPointButton");

        xSpin->setValue(x);
        ySpin->setValue(y);
        thetaSpin->setValue(theta);
        button->click();
        pumpEvents();
    }

    void addPath(QWidget *root, int fromId, int toId, int pathTypeIndex = 0, double sagitta = 1.0)
    {
        auto *startCombo = requireChild<QComboBox>(root, "mapPathStartCombo");
        auto *endCombo = requireChild<QComboBox>(root, "mapPathEndCombo");
        auto *typeCombo = requireChild<QComboBox>(root, "mapPathTypeCombo");
        auto *sagittaSpin = requireChild<QDoubleSpinBox>(root, "mapArcSagittaSpin");
        auto *button = requireChild<QPushButton>(root, "mapAddPathButton");

        setComboToPointId(startCombo, fromId);
        setComboToPointId(endCombo, toId);
        typeCombo->setCurrentIndex(pathTypeIndex);
        sagittaSpin->setValue(sagitta);
        button->click();
        pumpEvents();
    }

    void addRoute(QWidget *root, int fromId, int toId)
    {
        auto *startCombo = requireChild<QComboBox>(root, "mapRouteStartCombo");
        auto *endCombo = requireChild<QComboBox>(root, "mapRouteEndCombo");
        auto *button = requireChild<QPushButton>(root, "mapRouteAddButton");

        setComboToPointId(startCombo, fromId);
        setComboToPointId(endCombo, toId);
        button->click();
        pumpEvents();
    }

    void startRoute(QWidget *root)
    {
        auto *button = requireChild<QPushButton>(root, "mapRouteStartButton");
        QVERIFY(button->isEnabled());
        button->click();
        pumpEvents();
    }

    void removePath(QWidget *root, int fromId, int toId)
    {
        auto *table = requireChild<QTableWidget>(root, "mapPathTable");
        auto *button = requireChild<QPushButton>(root, "mapRemovePathButton");

        int rowToRemove = -1;
        for (int row = 0; row < table->rowCount(); ++row) {
            const QTableWidgetItem *startItem = table->item(row, 1);
            const QTableWidgetItem *endItem = table->item(row, 2);
            if (!startItem || !endItem) {
                continue;
            }
            if (startItem->text().toInt() == fromId && endItem->text().toInt() == toId) {
                rowToRemove = row;
                break;
            }
        }

        QVERIFY2(rowToRemove >= 0, "path row not found");
        table->selectRow(rowToRemove);
        button->click();
        pumpEvents();
    }


    bool polylineContains(const QList<QPointF> &polyline, const QPointF &expected) const
    {
        for (const QPointF &point : polyline) {
            if (std::abs(point.x() - expected.x()) < 1e-6
                && std::abs(point.y() - expected.y()) < 1e-6) {
                return true;
            }
        }
        return false;
    }

    void prepareCheckpointEditor(Map &map)
    {
        map.m_rowWorkPlan = {};
        map.m_rowWorkPlan.planId = "editor-regression";
        map.m_rowWorkPlan.frameBinding = map.m_binding;
        map.m_rowWorkPlan.hasStartPose = map.m_rowWorkPlan.hasEndPose = true;
        map.m_rowWorkPlan.endPose = {0, 4, 0};
        map.m_rowWorkPlan.checkpoints = {{"disabled-original", 1.23456789, 1500, false, true, false, true, 12000}};
        map.refreshRowWorkUi();
    }

    void prepareStartFirstDraft(Map &map)
    {
        map.m_binding.confirmed = true;
        map.m_binding.context = {"draft-map", "origin", "transform", "cal", "profile", 1};
        map.m_newMapMayAutoBind = false;
        map.m_rowWorkPlan = {};
        map.m_rowWorkPlan.frameBinding = map.m_binding;
        map.m_rowWorkPlan.planId = "start-first-draft";
        map.refreshRowWorkUi();
        map.syncCommittedMapState();
    }

    void applyTaughtPose(Map &map, Map::RowWorkPendingCaptureTarget target, const RowWorkPose &pose)
    {
        const MapFrameAdapter frame(map.m_binding);
        const auto enu = frame.toEnu(pose.toPointF());
        map.m_rowWorkPendingCaptureTarget = target;
        map.applyCapture({{"state", "succeeded"},
            {"originRevision", map.m_binding.context.originRevision},
            {"calibrationId", map.m_binding.context.calibrationId},
            {"pose", QJsonObject{{"x", enu.x()}, {"y", enu.y()}, {"yaw", frame.yawToEnu(pose.yaw)}}},
            {"sampleCount", 3}, {"positionScatterMeters", .002}});
    }

    void prepareRemoteTaskReview(Map &map, MapTaskReviewFixture &server,
                                ControlSessionCoordinator &coordinator)
    {
        map.m_binding.confirmed = true;
        map.m_binding.context = {"review-map", "origin", "enu", "cal", "profile", 1};
        map.m_newMapMayAutoBind = false;
        map.setControlCoordinator(&coordinator);
        map.m_planningFingerprint.clear();
        map.refreshPlanningRevision();
        map.syncCommittedMapState();
        server.plan = {{"schemaVersion", 1}, {"taskId", "running-task"}, {"revision", 1},
            {"context", map.m_binding.context.toJson()},
            {"steps", QJsonArray{QJsonObject{{"stepId", "wait"}, {"type", "wait"},
                {"completion", QJsonObject{{"type", "timer"}, {"durationMs", 1000}}}}}}};
    }

    void respondToMapDialogs(const std::function<void()> &operation,
                             QMessageBox::StandardButton answer, const QString &selectedFile = {})
    {
        bool sawConfirmation = false;
        bool sawFileDialog = selectedFile.isEmpty();
        bool timedOut = false;
        QElapsedTimer elapsed;
        elapsed.start();
        QTimer responder;
        connect(&responder, &QTimer::timeout, this, [&] {
            auto *modal = QApplication::activeModalWidget();
            if (elapsed.elapsed() > 3000) {
                timedOut = true;
                if (auto *dialog = qobject_cast<QDialog *>(modal)) dialog->reject();
                return;
            }
            if (auto *file = qobject_cast<QFileDialog *>(modal); file && !sawFileDialog) {
                sawFileDialog = true;
                file->setDirectory(QFileInfo(selectedFile).absolutePath());
                file->selectFile(selectedFile);
                QMetaObject::invokeMethod(file, "accept", Qt::QueuedConnection);
            } else if (auto *message = qobject_cast<QMessageBox *>(modal);
                       message && message->objectName() == "mapUnsavedChangesDialog") {
                sawConfirmation = true;
                QVERIFY(message->button(QMessageBox::Save));
                QVERIFY(message->button(QMessageBox::Discard));
                QVERIFY(message->button(QMessageBox::Cancel));
                message->button(answer)->click();
            }
        });
        responder.start(5);
        operation();
        responder.stop();
        QVERIFY(!timedOut);
        QVERIFY(sawFileDialog);
        QVERIFY(sawConfirmation);
    }

private slots:
    void showsTaskTabsForRouteAndRowWork();
    void removesRetiredMissionEditorsAndNestedModeTabs();
    void separatesPointPathAndMapEditors();
    void presentsSingleExecutionActionWithContextualRecoveryControls();
    void sharedTaskButtonRequiresReviewAndASecondExplicitClick_data();
    void sharedTaskButtonRequiresReviewAndASecondExplicitClick();
    void reviewCannotConfirmAChangedExecutionOrMap_data();
    void reviewCannotConfirmAChangedExecutionOrMap();
    void taskReviewCanRetryAfterAnOldExecutionReadOccupiedTheLane();
    void taskReviewIgnoresAReadReturnedAfterExecutionContinues();
    void photoReviewIsContextualAndRequiresExplicitResolution_data();
    void photoReviewIsContextualAndRequiresExplicitResolution();
    void photoReviewRejectsIdentityChangesWhileDialogIsOpen();
    void rowWorkHasNoDuplicateMotionParameterEditors();
    void editModeShowsSelectionAndCancelsPointPlacement();
    void editModeSelectionSurvivesDisabledPage();
    void checkpointEditorRejectsInvalidValues_data();
    void checkpointEditorRejectsInvalidValues();
    void checkpointEditorRetainsPrecisionFlagsAndSavedValues();
    void checkpointEditorKeepsSamePositionActionOrder();
    void straightTaskManualPlanningSaveReopenAndClear();
    void selectingCheckpointEnablesDeleteImmediately();
    void routeQueueRequiresSelectionAndConnectedSteps();
    void activeTaskDisablesPlanningAndDuplicateExecution();
    void editingAtMaximumLocalVersionCreatesANewIdentity();
    void loadingAndNewMapProtectUnsavedWork();
    void partialLineDraftIsDirtyAndSurvivesSaving();
    void endpointFirstDraftCanBeReopenedAndCompleted();
    void startFirstCheckpointsSurviveDraftSaveAndFinishOnTheFinalLine();
    void draftEndpointAndStartChangesCannotSilentlyMoveCheckpoints();
    void autoConnectsNewInputClickAndBatchPointsInOneDirection();
    void autoConnectionDoesNotBackfillLoadOrMakeZeroLengthPaths();
    void routePointDwellEditingPreservesPrecisionAndMapRoundTrip();
    void clickingNearRecordedPointOnlyChangesItsDwell();
    void switchingCurrentSelectedPointRefreshesItsEditors();
    void loadingMapAndReusingPointIdDoNotReuseOldSelection();
    void exhaustedMapIdsRejectNewObjectsWithoutChangingMap();
    void autoConnectionRejectsExhaustedPathIdsWithoutChangingMap();
    void invalidMapLoadPreservesCurrentMapAndEditors_data();
    void invalidMapLoadPreservesCurrentMapAndEditors();
    void routeSectionsIncludeIntermediateAndStartDwellOnce();
    void rightClickMenuRechecksTaskLockAfterNestedEvents_data();
    void rightClickMenuRechecksTaskLockAfterNestedEvents();
};

void MapRouteIntegrationTest::showsTaskTabsForRouteAndRowWork()
{
    QMainWindow window;
    Ui::MainWindow ui;
    ui.setupUi(&window);

    Map map(&ui);
    QWidget *mapPage = ui.mapPage;
    QVERIFY(mapPage != nullptr);

    auto *tabWidget = requireChild<QTabWidget>(mapPage, "mapTaskTabWidget");
    QVERIFY(tabWidget != nullptr);
    QCOMPARE(tabWidget->count(), 2);
    QCOMPARE(tabWidget->tabText(0), QStringLiteral("常规任务"));
    QCOMPARE(tabWidget->tabText(1), QStringLiteral("直线任务"));
}

void MapRouteIntegrationTest::presentsSingleExecutionActionWithContextualRecoveryControls()
{
    QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
    auto *page = ui.mapPage;
    QCOMPARE(requireChild<QPushButton>(page, "mapRouteStartButton")->text(), QStringLiteral("下发并执行"));
    QCOMPARE(requireChild<QPushButton>(page, "mapRowWorkStartButton")->text(), QStringLiteral("下发并执行"));
    QVERIFY(!page->findChild<QPushButton *>("mapMissionExecuteButton"));
    QVERIFY(!page->findChild<QMenu *>("mapTaskAdvancedMenu"));
    QVERIFY(!page->findChild<QAction *>("mapOnlyUploadAction"));
    auto *editors = requireChild<QTabWidget>(page, "mapEditorTabs");
    auto *coordinates = requireChild<QPushButton>(page, "mapFrameBindingButton");
    QVERIFY(editors->widget(2)->isAncestorOf(coordinates));
    QVERIFY(!editors->widget(0)->isAncestorOf(coordinates));
    QVERIFY(requireChild<QPushButton>(page, "mapCheckpointReviewButton")->isHidden());
    QVERIFY(requireChild<QPushButton>(page, "mapCancelPendingStartButton"));
}

void MapRouteIntegrationTest::sharedTaskButtonRequiresReviewAndASecondExplicitClick_data()
{
    QTest::addColumn<QString>("state");
    QTest::addColumn<bool>("acceptReview");
    for (const auto &state : {QString("Ready"), QString("Paused")}) {
        QTest::newRow((state + "-cancel").toUtf8().constData()) << state << false;
        QTest::newRow((state + "-confirm").toUtf8().constData()) << state << true;
    }
}

void MapRouteIntegrationTest::sharedTaskButtonRequiresReviewAndASecondExplicitClick()
{
    QFETCH(QString, state);
    QFETCH(bool, acceptReview);
    MapTaskReviewFixture server; server.state = state;
    if (state == "Ready") server.execution.clear();
    QVERIFY(server.isListening());
    TrackingClient tracking; PoseClient pose; ControlSessionCoordinator coordinator(&tracking, &pose);
    QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
    prepareRemoteTaskReview(map, server, coordinator);
    auto steps = server.plan["steps"].toArray();
    steps.prepend(QJsonObject{{"stepId", "return"}, {"type", "return_to_start"},
        {"points", QJsonArray{QJsonArray{2, 0}, QJsonArray{0, 0}}}, {"speedLimit", .15},
        {"goalToleranceMeters", .1}, {"safetyProfileId", "field-low-speed"}, {"endBehavior", "stop"}});
    steps.append(QJsonObject{{"stepId", "work"}, {"type", "follow_path"},
        {"points", QJsonArray{QJsonArray{0, 0}, QJsonArray{2, 0}}}, {"speedLimit", .15},
        {"goalToleranceMeters", .1}, {"safetyProfileId", "field-low-speed"}, {"endBehavior", "stop"}});
    server.plan["steps"] = steps;
    tracking.configure(server.url(), {}, 500); tracking.start();
    QTRY_VERIFY(tracking.commandFresh());
    QTRY_VERIFY(coordinator.coordinatesReady());
    auto *button = requireChild<QPushButton>(ui.mapPage, "mapTaskResumeButton");
    QTRY_VERIFY(button->isEnabled());
    QCOMPARE(button->text(), state == "Ready" ? QStringLiteral("启动任务") : QStringLiteral("继续任务"));
    bool reviewed = false, eligible = false, returnStepExplained = false;
    QTimer responder;
    connect(&responder, &QTimer::timeout, this, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog || dialog->objectName() != "remoteTaskReviewDialog") return;
        reviewed = true;
        auto *steps = dialog->findChild<QListWidget *>("remoteTaskSteps");
        returnStepExplained = steps && steps->count() == 3
            && steps->item(0)->text().contains(QStringLiteral("先沿路径返回起点"))
            && !steps->item(0)->text().contains("return_to_start");
        auto *verified = dialog->findChild<QCheckBox *>("remoteTaskVerified");
        auto *buttons = dialog->findChild<QDialogButtonBox *>();
        eligible = verified && verified->isEnabled() && buttons;
        if (acceptReview && eligible) {
            verified->setChecked(true);
            buttons->button(QDialogButtonBox::Ok)->click();
        } else dialog->reject();
    });
    responder.start(5);
    button->click();
    button->click(); // A rapid duplicate click cannot queue another read or motion.
    QTRY_VERIFY(reviewed);
    responder.stop();
    QVERIFY(eligible);
    QVERIFY(returnStepExplained);
    QCOMPARE(server.taskReads, 1);
    QVERIFY(server.controls.isEmpty());
    if (acceptReview) {
        QTRY_COMPARE(tracking.preparedPlan(), server.plan);
        QTRY_VERIFY(!coordinator.actionPending());
        QTRY_VERIFY(tracking.motionReady());
        QVERIFY(button->isEnabled());
        button->click();
        button->click();
        QTRY_COMPARE(server.controls.size(), 1);
        QCOMPARE(server.controls.first(), state == "Ready" ? QString("start") : QString("resume"));
        QTRY_VERIFY(!tracking.busy());
        QCOMPARE(server.taskReads, 1);
    } else {
        QVERIFY(tracking.preparedPlan().isEmpty());
        QVERIFY(!coordinator.actionPending());
        QTRY_VERIFY(button->isEnabled());
        QCOMPARE(server.sessions, 0);
    }
    coordinator.shutdown(); tracking.stop();
}

void MapRouteIntegrationTest::reviewCannotConfirmAChangedExecutionOrMap_data()
{
    QTest::addColumn<bool>("changeExecution");
    QTest::newRow("execution-changed-during-review") << true;
    QTest::newRow("map-changed-during-review") << false;
}

void MapRouteIntegrationTest::reviewCannotConfirmAChangedExecutionOrMap()
{
    QFETCH(bool, changeExecution);
    MapTaskReviewFixture server;
    TrackingClient tracking; PoseClient pose; ControlSessionCoordinator coordinator(&tracking, &pose);
    QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
    prepareRemoteTaskReview(map, server, coordinator);
    tracking.configure(server.url(), {}, 500); tracking.start();
    QTRY_VERIFY(coordinator.coordinatesReady());
    auto *button = requireChild<QPushButton>(ui.mapPage, "mapTaskResumeButton");
    QTRY_VERIFY(button->isEnabled());
    bool opened = false, closed = false, changed = false, timedOut = false;
    QElapsedTimer reviewAge;
    QTimer responder;
    connect(&responder, &QTimer::timeout, this, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog || dialog->objectName() != "remoteTaskReviewDialog") return;
        if (reviewAge.isValid() && reviewAge.elapsed() > 3000) {
            timedOut = true;
            dialog->reject();
            return;
        }
        if (!opened) {
            opened = true; reviewAge.start();
            connect(dialog, &QDialog::finished, &responder, [&](int) { closed = true; });
            if (changeExecution) server.execution = "different-execution";
            else {
                map.m_binding.context.mapRevision += 1;
                coordinator.setBinding(map.m_binding);
                changed = true;
            }
        }
        if (changeExecution && tracking.snapshot().executionId == server.execution) changed = true;
        if (!changed) return;
        auto *verified = dialog->findChild<QCheckBox *>("remoteTaskVerified");
        auto *buttons = dialog->findChild<QDialogButtonBox *>();
        if (verified && verified->isEnabled() && buttons) {
            verified->setChecked(true);
            buttons->button(QDialogButtonBox::Ok)->click();
        } else dialog->reject();
    });
    responder.start(5);
    button->click();
    QTRY_VERIFY(closed);
    responder.stop();
    QVERIFY(opened);
    QVERIFY(!timedOut);
    QVERIFY(changed || (changeExecution && tracking.snapshot().executionId == server.execution));
    QVERIFY(tracking.preparedPlan().isEmpty());
    QVERIFY(server.controls.isEmpty());
    QVERIFY(!coordinator.actionPending());
    QCOMPARE(server.taskReads, 1);
    coordinator.shutdown(); tracking.stop();
}

void MapRouteIntegrationTest::taskReviewCanRetryAfterAnOldExecutionReadOccupiedTheLane()
{
    MapTaskReviewFixture server;
    server.holdNextTaskRead = true;
    TrackingClient tracking; PoseClient pose; ControlSessionCoordinator coordinator(&tracking, &pose);
    QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
    prepareRemoteTaskReview(map, server, coordinator);
    tracking.configure(server.url(), {}, 5000); tracking.start();
    QTRY_VERIFY(coordinator.coordinatesReady());
    auto *button = requireChild<QPushButton>(ui.mapPage, "mapTaskResumeButton");
    QTRY_VERIFY(button->isEnabled());
    button->click();
    QTRY_COMPARE(server.taskReads, 1);
    QVERIFY(server.heldTaskRead);
    QVERIFY(!button->isEnabled());

    // Another operator changes the execution while the old wireless reply is
    // still outstanding. A fresh click must not leave a phantom pending read.
    server.execution = "different-execution";
    QTRY_COMPARE(tracking.snapshot().executionId, server.execution);
    QTRY_VERIFY(button->isEnabled());
    button->click();
    QVERIFY(button->isEnabled());
    QCOMPARE(server.taskReads, 1);
    QVERIFY(tracking.preparedPlan().isEmpty());
    QVERIFY(server.controls.isEmpty());
    QSignalSpy readbacks(&tracking, &TrackingClient::taskReceived);
    server.releaseHeldTaskRead();

    bool reviewed = false;
    QTimer responder, retry;
    connect(&responder, &QTimer::timeout, this, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog || dialog->objectName() != "remoteTaskReviewDialog") return;
        reviewed = true;
        retry.stop();
        dialog->reject();
    });
    // Model an explicit retry once the shared HTTP lane has completed. Failed
    // lane acquisitions remain visible and retryable; none are motion writes.
    connect(&retry, &QTimer::timeout, this, [&] {
        if (server.taskReads < 2 && button->isEnabled()) button->click();
    });
    responder.start(5); retry.start(50);
    QTRY_VERIFY(reviewed);
    responder.stop(); retry.stop();
    QCOMPARE(server.taskReads, 2);
    QCOMPARE(readbacks.size(), 1); // The response for the previous execution is ignored.
    QVERIFY(button->isEnabled());
    QVERIFY(tracking.preparedPlan().isEmpty());
    QVERIFY(server.controls.isEmpty());
    QCOMPARE(server.sessions, 0);
    coordinator.shutdown(); tracking.stop();
}

void MapRouteIntegrationTest::taskReviewIgnoresAReadReturnedAfterExecutionContinues()
{
    MapTaskReviewFixture server;
    server.holdNextTaskRead = true;
    TrackingClient tracking; PoseClient pose; ControlSessionCoordinator coordinator(&tracking, &pose);
    QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
    prepareRemoteTaskReview(map, server, coordinator);
    tracking.configure(server.url(), {}, 5000); tracking.start();
    QTRY_VERIFY(coordinator.coordinatesReady());
    auto *button = requireChild<QPushButton>(ui.mapPage, "mapTaskResumeButton");
    QTRY_VERIFY(button->isEnabled());
    button->click();
    QTRY_COMPARE(server.taskReads, 1);
    QVERIFY(server.heldTaskRead);

    // The IPC may continue the same execution while a read is on the wireless
    // link. Its late response must not interrupt monitoring with a stale modal.
    server.state = "Tracking";
    QTRY_COMPARE(tracking.snapshot().state, server.state);
    QVERIFY(!button->isEnabled());
    int opened = 0;
    QTimer responder;
    connect(&responder, &QTimer::timeout, this, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog || dialog->objectName() != "remoteTaskReviewDialog") return;
        ++opened;
        dialog->reject();
    });
    responder.start(5);
    QSignalSpy readbacks(&tracking, &TrackingClient::taskReceived);
    server.releaseHeldTaskRead();
    QTRY_COMPARE(readbacks.size(), 1);
    QCOMPARE(opened, 0);
    QVERIFY(tracking.preparedPlan().isEmpty());
    QVERIFY(server.controls.isEmpty());

    server.state = "Paused";
    QTRY_COMPARE(tracking.snapshot().state, server.state);
    QTRY_VERIFY(button->isEnabled());
    QCOMPARE(opened, 0);
    button->click();
    QTRY_COMPARE(opened, 1);
    responder.stop();
    QCOMPARE(server.taskReads, 2);
    QCOMPARE(readbacks.size(), 2);
    QVERIFY(button->isEnabled());
    QVERIFY(tracking.preparedPlan().isEmpty());
    QVERIFY(server.controls.isEmpty());
    QCOMPARE(server.sessions, 0);
    coordinator.shutdown(); tracking.stop();
}

void MapRouteIntegrationTest::photoReviewIsContextualAndRequiresExplicitResolution_data()
{
    QTest::addColumn<QString>("state");
    QTest::addColumn<int>("answer");
    for (const auto &state : {QString("Waiting"), QString("Paused")})
        for (const auto answer : {QMessageBox::Yes, QMessageBox::No, QMessageBox::Cancel})
            QTest::newRow((state + '-' + QString::number(answer)).toUtf8().constData()) << state << int(answer);
}

void MapRouteIntegrationTest::photoReviewIsContextualAndRequiresExplicitResolution()
{
    QFETCH(QString, state);
    QFETCH(int, answer);
    MapTaskReviewFixture server; server.state = state; server.waitingEvent = "photo-event";
    TrackingClient tracking; PoseClient pose; ControlSessionCoordinator coordinator(&tracking, &pose);
    QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
    prepareRemoteTaskReview(map, server, coordinator);
    auto *button = requireChild<QPushButton>(ui.mapPage, "mapCheckpointReviewButton");
    QVERIFY(button->isHidden());
    tracking.configure(server.url(), {}, 500); tracking.start();
    QTRY_VERIFY(tracking.commandFresh());
    map.setExternalActionReviewEvent("different-event");
    QVERIFY(button->isHidden());
    map.setExternalActionReviewEvent(server.waitingEvent);
    QTRY_VERIFY(!button->isHidden());
    QTRY_VERIFY(button->isEnabled());
    QSignalSpy resolutions(&map, &Map::externalActionResolved);
    bool reviewed = false;
    QTimer responder;
    connect(&responder, &QTimer::timeout, this, [&] {
        auto *message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (!message || !message->button(QMessageBox::Yes) || !message->button(QMessageBox::No)) return;
        reviewed = true;
        message->button(static_cast<QMessageBox::StandardButton>(answer))->click();
    });
    responder.start(5);
    button->click(); button->click();
    QTRY_VERIFY(reviewed);
    responder.stop();
    QCOMPARE(server.sessions, 1);
    QCOMPARE(resolutions.size(), answer == QMessageBox::Cancel ? 0 : 1);
    if (!resolutions.isEmpty()) {
        QCOMPARE(resolutions.first().at(0).toString(), server.waitingEvent);
        QCOMPARE(resolutions.first().at(1).toBool(), answer == QMessageBox::Yes);
    }
    QVERIFY(server.controls.isEmpty());
    server.state = "Tracking";
    QTRY_COMPARE(tracking.snapshot().state, server.state);
    QVERIFY(button->isHidden());
    coordinator.shutdown(); tracking.stop();
}

void MapRouteIntegrationTest::photoReviewRejectsIdentityChangesWhileDialogIsOpen()
{
    MapTaskReviewFixture server; server.state = "Waiting"; server.waitingEvent = "photo-event";
    TrackingClient tracking; PoseClient pose; ControlSessionCoordinator coordinator(&tracking, &pose);
    QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
    prepareRemoteTaskReview(map, server, coordinator);
    tracking.configure(server.url(), {}, 500); tracking.start();
    QTRY_VERIFY(tracking.commandFresh());
    map.setExternalActionReviewEvent(server.waitingEvent);
    auto *button = requireChild<QPushButton>(ui.mapPage, "mapCheckpointReviewButton");
    QTRY_VERIFY(button->isEnabled());
    QSignalSpy resolutions(&map, &Map::externalActionResolved);
    bool opened = false, closed = false, timedOut = false;
    QElapsedTimer reviewAge;
    QTimer responder;
    connect(&responder, &QTimer::timeout, this, [&] {
        auto *message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (!message || !message->button(QMessageBox::Yes)) return;
        if (reviewAge.isValid() && reviewAge.elapsed() > 3000) {
            timedOut = true;
            message->reject();
            return;
        }
        if (!opened) {
            opened = true; reviewAge.start();
            connect(message, &QDialog::finished, &responder, [&](int) { closed = true; });
            server.execution = "different-execution";
        }
        if (tracking.snapshot().executionId == server.execution) message->button(QMessageBox::Yes)->click();
    });
    responder.start(5);
    button->click();
    QTRY_VERIFY(closed);
    responder.stop();
    QVERIFY(opened);
    QVERIFY(!timedOut);
    QVERIFY(resolutions.isEmpty());
    QVERIFY(server.controls.isEmpty());
    coordinator.shutdown(); tracking.stop();
}

void MapRouteIntegrationTest::rowWorkHasNoDuplicateMotionParameterEditors()
{
    QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
    for (const char *name : {"mapRowWorkBaseSpeedSpin", "mapRowWorkMaxSpeedSpin",
             "mapRowWorkEndpointArrivalSpin", "mapRowWorkCheckpointToleranceSpin",
             "mapRowWorkTurnAngularSpeedSpin"})
        QVERIFY(!ui.mapPage->findChild<QDoubleSpinBox *>(QString::fromLatin1(name)));
    QVERIFY(requireChild<QCheckBox>(ui.mapPage, "mapRowWorkLoopCheck"));
}

void MapRouteIntegrationTest::checkpointEditorRejectsInvalidValues_data()
{
    QTest::addColumn<int>("column");
    QTest::addColumn<QString>("text");
    for (const auto &text : {QString(), QString("invalid"), QString("nan"), QString("inf"),
                             QString("-0.01"), QString("4.0001"), QString("1e309")})
        QTest::newRow(("distance-" + text).toUtf8().constData()) << 1 << text;
    for (const auto &text : {QString(), QString("invalid"), QString("1.5"), QString("-1"),
                             QString("3600001"), QString("999999999999999999999")})
        QTest::newRow(("dwell-" + text).toUtf8().constData()) << 2 << text;
    for (const auto &text : {QString(), QString("invalid"), QString("1.5"), QString("999"), QString("3600001")})
        QTest::newRow(("timeout-" + text).toUtf8().constData()) << 6 << text;
}

void MapRouteIntegrationTest::checkpointEditorRejectsInvalidValues()
{
    QFETCH(int, column);
    QFETCH(QString, text);
    QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
    prepareCheckpointEditor(map);
    auto *table = requireChild<QTableWidget>(ui.mapPage, "mapRowWorkCheckpointTable");
    const auto before = map.serializeMap()["rowWork"].toObject();
    const auto originalText = table->item(0, column)->text();
    table->item(0, column)->setText(text);
    pumpEvents();
    QCOMPARE(map.serializeMap()["rowWork"].toObject(), before);
    QCOMPARE(table->item(0, column)->text(), originalText);
    QVERIFY(requireChild<QLabel>(ui.mapPage, "mapTaskNoticeLabel")->text().contains(QStringLiteral("已恢复原值")));
}

void MapRouteIntegrationTest::checkpointEditorRetainsPrecisionFlagsAndSavedValues()
{
    QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
    prepareCheckpointEditor(map);
    auto *table = requireChild<QTableWidget>(ui.mapPage, "mapRowWorkCheckpointTable");
    const double progress = map.m_rowWorkPlan.checkpoints[0].progress;
    for (const auto &text : {QString("0"), QString("3600000"), QString("2300")}) {
        table->item(0, 2)->setText(text);
        pumpEvents();
        QCOMPARE(map.m_rowWorkPlan.checkpoints[0].dwellMs, text.toInt());
        QCOMPARE(map.m_rowWorkPlan.checkpoints[0].progress, progress);
        QVERIFY(!map.m_rowWorkPlan.checkpoints[0].enabled);
    }
    for (const auto &text : {QString("1000"), QString("3600000"), QString("15000")}) {
        table->item(0, 6)->setText(text);
        pumpEvents();
        QCOMPARE(map.m_rowWorkPlan.checkpoints[0].actionTimeoutMs, text.toInt());
        QCOMPARE(map.m_rowWorkPlan.checkpoints[0].progress, progress);
    }
    table->item(0, 0)->setText("renamed");
    table->item(0, 3)->setCheckState(Qt::Unchecked);
    table->item(0, 4)->setCheckState(Qt::Checked);
    table->item(0, 5)->setCheckState(Qt::Unchecked);
    QCOMPARE(map.m_rowWorkPlan.checkpoints[0].progress, progress);
    QVERIFY(!map.m_rowWorkPlan.checkpoints[0].enabled);
    QVERIFY(!map.m_rowWorkPlan.checkpoints[0].triggerOnForward);
    QVERIFY(map.m_rowWorkPlan.checkpoints[0].triggerOnBackward);
    QVERIFY(!map.m_rowWorkPlan.checkpoints[0].capturePhoto);
    for (const auto &text : {QString("0"), QString("4"), QString("1.123456789")}) {
        table->item(0, 1)->setText(text);
        pumpEvents();
        QCOMPARE(map.m_rowWorkPlan.checkpoints[0].progress, text.toDouble());
    }
    const auto beforeSave = RowWorkJson::planToJson(map.m_rowWorkPlan);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString filePath = directory.filePath("edited-map.json");
    QVERIFY(map.saveMapToFile(filePath));
    QFile saved(filePath);
    QVERIFY(saved.open(QIODevice::ReadOnly));
    const auto document = QJsonDocument::fromJson(saved.readAll()).object();
    QCOMPARE(document["rowWork"].toObject(), beforeSave);
    map.m_rowWorkPlan.checkpoints.clear();
    QVERIFY(map.deserializeMap(document));
    QCOMPARE(RowWorkJson::planToJson(map.m_rowWorkPlan), beforeSave);
    QCOMPARE(table->item(0, 1)->text(), QString::number(1.123456789, 'f', 2));
    table->item(0, 2)->setText("2500");
    QCOMPARE(map.m_rowWorkPlan.checkpoints[0].progress, 1.123456789);
    QCOMPARE(map.m_rowWorkPlan.checkpoints[0].dwellMs, 2500);
    QVERIFY(!map.m_rowWorkPlan.checkpoints[0].enabled);
}

void MapRouteIntegrationTest::checkpointEditorKeepsSamePositionActionOrder()
{
    QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
    prepareCheckpointEditor(map);
    map.m_rowWorkPlan.checkpoints.clear();
    QStringList order;
    for (int i = 0; i < 24; ++i) {
        const QString name = QString("action-%1").arg(i);
        order.append(name);
        map.m_rowWorkPlan.checkpoints.append({name, 1, 1000 + i, true, true, true, i % 2 == 0, 12000});
    }
    map.refreshRowWorkUi();
    auto *table = requireChild<QTableWidget>(ui.mapPage, "mapRowWorkCheckpointTable");
    table->item(10, 2)->setText("3333");
    QStringList actual;
    for (const auto &checkpoint : map.m_rowWorkPlan.checkpoints) actual.append(checkpoint.name);
    QCOMPARE(actual, order);
    QCOMPARE(map.m_rowWorkPlan.checkpoints[10].dwellMs, 3333);
    QCOMPARE(table->item(10, 0)->text(), "action-10");
}

void MapRouteIntegrationTest::straightTaskManualPlanningSaveReopenAndClear()
{
    QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
    prepareCheckpointEditor(map);
    map.m_rowWorkPlan.checkpoints.clear();
    map.refreshRowWorkUi();
    ui.stackedWidget->setCurrentWidget(ui.mapPage);
    requireChild<QTabWidget>(ui.mapPage, "mapTaskTabWidget")->setCurrentIndex(1);
    window.resize(1600, 1000);
    window.show();
    pumpEvents();
    auto *add = requireChild<QPushButton>(ui.mapPage, "mapRowWorkAddCheckpointFromMapButton");
    auto *loop = requireChild<QCheckBox>(ui.mapPage, "mapRowWorkLoopCheck");
    auto *table = requireChild<QTableWidget>(ui.mapPage, "mapRowWorkCheckpointTable");
    auto *view = requireChild<MapGraphicsView>(ui.mapPage, "mapGraphicsView");
    auto *scroll = requireChild<QScrollArea>(ui.mapPage, "mapRowWorkPrimitiveScrollArea");
    QVERIFY(loop->isChecked());
    loop->click();
    QVERIFY(!map.m_rowWorkPlan.params.loopEnabled);
    loop->click();
    QVERIFY(map.m_rowWorkPlan.params.loopEnabled);
    loop->click();
    QVERIFY(!map.m_rowWorkPlan.params.loopEnabled);
    view->resetTransform();
    view->centerOn(map.mapToScene({0, 2}));
    for (double progress : {1., 3.}) {
        QVERIFY(add->isEnabled());
        add->click();
        QVERIFY(add->isChecked());
        const QPoint click = view->mapFromScene(map.mapToScene({0, progress}));
        QVERIFY(view->viewport()->rect().contains(click));
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, click);
        pumpEvents();
        QVERIFY(!add->isChecked());
    }
    QCOMPARE(table->rowCount(), 2);
    QVERIFY(std::abs(map.m_rowWorkPlan.checkpoints[0].progress - 1) < .03);
    QVERIFY(std::abs(map.m_rowWorkPlan.checkpoints[1].progress - 3) < .03);
    scroll->ensureWidgetVisible(table);
    table->setCurrentCell(0, 2);
    table->editItem(table->item(0, 2));
    auto *editor = table->findChild<QLineEdit *>();
    QVERIFY(editor);
    editor->selectAll();
    QTest::keyClicks(editor, "1250");
    QTest::keyClick(editor, Qt::Key_Return);
    pumpEvents();
    QCOMPARE(map.m_rowWorkPlan.checkpoints[0].dwellMs, 1250);
    for (int column : {3, 4, 5}) {
        table->setCurrentCell(0, column);
        QTest::keyClick(table, Qt::Key_Space);
        pumpEvents();
    }
    QVERIFY(!map.m_rowWorkPlan.checkpoints[0].triggerOnForward);
    QVERIFY(!map.m_rowWorkPlan.checkpoints[0].triggerOnBackward);
    QVERIFY(map.m_rowWorkPlan.checkpoints[0].capturePhoto);
    table->setCurrentCell(0, 4);
    QTest::keyClick(table, Qt::Key_Space);
    QVERIFY(map.m_rowWorkPlan.checkpoints[0].triggerOnBackward);

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString filePath = directory.filePath("manual-plan.json");
    map.m_currentMapFilePath = filePath; // A map already opened by the operator.
    requireChild<QPushButton>(ui.mapPage, "mapQuickSaveButton")->click();
    QFile saved(filePath);
    QVERIFY(saved.open(QIODevice::ReadOnly));
    const auto written = QJsonDocument::fromJson(saved.readAll()).object()["rowWork"].toObject();
    saved.close();
    QVERIFY(!written["params"].toObject()["loopEnabled"].toBool());

    auto *remove = requireChild<QPushButton>(ui.mapPage, "mapRowWorkDeleteCheckpointButton");
    table->setCurrentCell(1, 0);
    QVERIFY(remove->isEnabled());
    remove->click();
    QCOMPARE(table->rowCount(), 1);
    QCOMPARE(map.m_rowWorkPlan.checkpoints[0].dwellMs, 1250);
    auto *clear = requireChild<QPushButton>(ui.mapPage, "mapRowWorkClearCheckpointsButton");
    clear->click();
    QCOMPARE(table->rowCount(), 0);
    QVERIFY(!remove->isEnabled());
    QVERIFY(!clear->isEnabled());
    const int clearedVersion = map.m_rowWorkPlan.version;
    clear->click();
    QCOMPARE(map.m_rowWorkPlan.version, clearedVersion);
    requireChild<QPushButton>(ui.mapPage, "mapRowWorkClearLineButton")->click();
    QVERIFY(!map.m_rowWorkPlan.hasStartPose);
    QVERIFY(!map.m_rowWorkPlan.hasEndPose);
    QVERIFY(!add->isEnabled());
    QVERIFY(map.loadMapFromFile(filePath));
    QCOMPARE(RowWorkJson::planToJson(map.m_rowWorkPlan), written);
    QCOMPARE(table->rowCount(), 2);
    QVERIFY(!loop->isChecked());
    QCOMPARE(table->item(0, 2)->text(), "1250");
    QCOMPARE(table->item(0, 3)->checkState(), Qt::Unchecked);
    QCOMPARE(table->item(0, 4)->checkState(), Qt::Checked);
    QCOMPARE(table->item(0, 5)->checkState(), Qt::Checked);
}

void MapRouteIntegrationTest::selectingCheckpointEnablesDeleteImmediately()
{
    QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
    prepareCheckpointEditor(map);
    auto *table = requireChild<QTableWidget>(ui.mapPage, "mapRowWorkCheckpointTable");
    auto *remove = requireChild<QPushButton>(ui.mapPage, "mapRowWorkDeleteCheckpointButton");
    table->setCurrentCell(-1, -1);
    QVERIFY(!remove->isEnabled());
    table->setCurrentCell(0, 0);
    QVERIFY(remove->isEnabled());
    remove->click();
    QCOMPARE(table->rowCount(), 0);
    QVERIFY(map.m_rowWorkPlan.checkpoints.isEmpty());
    QVERIFY(!remove->isEnabled());
    const int version = map.m_rowWorkPlan.version;
    remove->click();
    QCOMPARE(map.m_rowWorkPlan.version, version);
}

void MapRouteIntegrationTest::activeTaskDisablesPlanningAndDuplicateExecution()
{
    MapMonitorFixture server;
    QVERIFY(server.isListening());
    TrackingClient tracking; PoseClient pose; ControlSessionCoordinator coordinator(&tracking, &pose);
    QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
    map.clearMapData();
    QVERIFY(map.addPointInternal(0, 0, 0, 1));
    QVERIFY(map.addPointInternal(1, 0, 0, 2));
    QVERIFY(map.addLinePath(1, 2));
    map.refreshSelectors();
    addRoute(ui.mapPage, 1, 2);
    prepareCheckpointEditor(map);
    map.setControlCoordinator(&coordinator);
    auto *routeLoop = requireChild<QCheckBox>(ui.mapPage, "mapRouteLoopCheck");
    auto *routeInterval = requireChild<QSpinBox>(ui.mapPage, "mapRouteLoopIntervalSpin");
    routeLoop->click();
    QVERIFY(routeLoop->isChecked());
    routeInterval->setValue(7);
    tracking.configure(server.url(), {}, 500);
    tracking.start();
    QTRY_VERIFY(tracking.connected());
    auto *table = requireChild<QTableWidget>(ui.mapPage, "mapRowWorkCheckpointTable");
    table->setCurrentCell(0, 0);
    const auto before = RowWorkJson::planToJson(map.m_rowWorkPlan);
    auto *autoConnect = requireChild<QCheckBox>(ui.mapPage, "mapAutoConnectPointsCheck");
    auto *pointDwell = requireChild<QSpinBox>(ui.mapPage, "mapPointDwellSpin");
    auto *loop = requireChild<QCheckBox>(ui.mapPage, "mapRowWorkLoopCheck");
    const QStringList disabledButtons{"mapRowWorkAddCheckpointFromMapButton", "mapRowWorkDeleteCheckpointButton",
        "mapRowWorkClearCheckpointsButton", "mapRowWorkClearLineButton", "mapRowWorkStartButton",
        "mapRouteAddButton", "mapRouteRemoveButton", "mapRouteClearButton", "mapRouteStartButton",
        "mapNewButton", "mapLoadButton"};
    for (const auto &state : {QString("Tracking"), QString("Turning"), QString("Waiting"), QString("Paused")}) {
        server.state = state;
        QTRY_COMPARE(tracking.snapshot().state, state);
        QVERIFY(!table->isEnabled());
        QVERIFY(!loop->isEnabled());
        QVERIFY(!routeLoop->isEnabled());
        QVERIFY(!routeInterval->isEnabled());
        QVERIFY(!autoConnect->isEnabled());
        QVERIFY(!pointDwell->isEnabled());
        QVERIFY(!requireChild<QComboBox>(ui.mapPage, "mapRouteStartCombo")->isEnabled());
        QVERIFY(!requireChild<QComboBox>(ui.mapPage, "mapRouteEndCombo")->isEnabled());
        loop->click();
        routeLoop->click();
        QTest::keyClick(routeInterval, Qt::Key_Up);
        for (const auto &name : disabledButtons) {
            auto *button = requireChild<QPushButton>(ui.mapPage, name.toLatin1().constData());
            QVERIFY(!button->isEnabled());
            button->click();
            button->click();
        }
        QCOMPARE(RowWorkJson::planToJson(map.m_rowWorkPlan), before);
        QVERIFY(routeLoop->isChecked());
        QCOMPARE(routeInterval->value(), 7);
        map.handleRouteClear(); // A late queued slot also cannot edit the active plan.
        map.handleRouteRemove();
        map.handleRouteAdd();
        const auto pointCount = map.m_points.size();
        map.handleAddPointFromInput();
        map.handleBatchGeneratePoints();
        QCOMPARE(map.m_points.size(), pointCount);
        QCOMPARE(map.m_routeQueue.size(), 1);
        QCOMPARE(server.writes, 0);
    }
    server.state = "Completed";
    QTRY_COMPARE(tracking.snapshot().state, QString("Completed"));
    QVERIFY(table->isEnabled());
    QVERIFY(loop->isEnabled());
    QVERIFY(routeLoop->isEnabled());
    QVERIFY(routeInterval->isEnabled());
    QVERIFY(requireChild<QPushButton>(ui.mapPage, "mapNewButton")->isEnabled());
    QVERIFY(requireChild<QPushButton>(ui.mapPage, "mapLoadButton")->isEnabled());
    QVERIFY(requireChild<QPushButton>(ui.mapPage, "mapRowWorkClearCheckpointsButton")->isEnabled());
    loop->click();
    QVERIFY(!map.m_rowWorkPlan.params.loopEnabled);
    QCOMPARE(server.writes, 0);
    coordinator.shutdown();
    tracking.stop();
}

void MapRouteIntegrationTest::routeQueueRequiresSelectionAndConnectedSteps()
{
    MapMonitorFixture server;
    server.state = "Completed";
    QVERIFY(server.isListening());
    TrackingClient tracking; PoseClient pose; ControlSessionCoordinator coordinator(&tracking, &pose);
    QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
    map.clearMapData();
    for (int id = 1; id <= 4; ++id) QVERIFY(map.addPointInternal(id - 1, 0, 0, id));
    for (int id = 1; id < 4; ++id) QVERIFY(map.addLinePath(id, id + 1));
    map.refreshSelectors();
    map.setControlCoordinator(&coordinator);
    tracking.configure(server.url(), {}, 500);
    tracking.start();
    QTRY_VERIFY(tracking.commandFresh());
    auto *queue = requireChild<QListWidget>(ui.mapPage, "mapRouteQueueList");
    auto *remove = requireChild<QPushButton>(ui.mapPage, "mapRouteRemoveButton");
    auto *start = requireChild<QPushButton>(ui.mapPage, "mapRouteStartButton");
    auto *hint = requireChild<QLabel>(ui.mapPage, "mapRouteStartHintLabel");
    auto *empty = requireChild<QLabel>(ui.mapPage, "mapRouteQueueEmptyLabel");
    QVERIFY(!empty->isHidden());
    QVERIFY(!remove->isEnabled());
    for (int id = 1; id < 4; ++id) addRoute(ui.mapPage, id, id + 1);
    QCOMPARE(queue->count(), 3);
    QCOMPARE(queue->currentRow(), 2);
    QVERIFY(empty->isHidden());
    QVERIFY(start->isEnabled());
    queue->setCurrentRow(-1);
    QVERIFY(!remove->isEnabled());
    queue->setCurrentRow(1);
    QVERIFY(remove->isEnabled());
    remove->click();
    QCOMPARE(queue->count(), 2);
    QCOMPARE(queue->currentRow(), 1);
    QVERIFY(!start->isEnabled());
    QVERIFY(hint->text().contains(QStringLiteral("不衔接")));
    QCOMPARE(map.m_routeQueue[0].toId, 2);
    QCOMPARE(map.m_routeQueue[1].fromId, 3); // No implicit repair or rerouting.
    remove->click();
    QVERIFY(start->isEnabled());
    auto *loop = requireChild<QCheckBox>(ui.mapPage, "mapRouteLoopCheck");
    loop->click();
    QVERIFY(!start->isEnabled());
    QVERIFY(hint->text().contains(QStringLiteral("接回起点")));
    loop->click();
    QVERIFY(start->isEnabled());
    requireChild<QPushButton>(ui.mapPage, "mapRouteClearButton")->click();
    QVERIFY(!empty->isHidden());
    QVERIFY(!remove->isEnabled());
    QVERIFY(!start->isEnabled());
    QCOMPARE(server.writes, 0);
    coordinator.shutdown();
    tracking.stop();
}

void MapRouteIntegrationTest::editingAtMaximumLocalVersionCreatesANewIdentity()
{
    QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
    prepareCheckpointEditor(map);
    map.m_rowWorkPlan.version = std::numeric_limits<int>::max();
    const QString previousId = map.m_rowWorkPlan.planId;
    auto *table = requireChild<QTableWidget>(ui.mapPage, "mapRowWorkCheckpointTable");
    table->item(0, 2)->setText("2500");
    QCOMPARE(map.m_rowWorkPlan.version, 1);
    QVERIFY(!map.m_rowWorkPlan.planId.isEmpty());
    QVERIFY(map.m_rowWorkPlan.planId != previousId);
    QCOMPARE(map.m_rowWorkPlan.checkpoints[0].dwellMs, 2500);
    table->item(0, 2)->setText("3000");
    QCOMPARE(map.m_rowWorkPlan.version, 2);
}

void MapRouteIntegrationTest::loadingAndNewMapProtectUnsavedWork()
{
    NonNativeFileDialogs dialogs;
    QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
    prepareCheckpointEditor(map);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString currentFile = directory.filePath("current-map.json");
    const QString alternateFile = directory.filePath("alternate-map.json");
    map.m_currentMapFilePath = currentFile;
    requireChild<QPushButton>(ui.mapPage, "mapQuickSaveButton")->click();
    QVERIFY(!map.hasUnsavedMapChanges());
    auto alternate = map.serializeMap();
    auto row = alternate["rowWork"].toObject();
    row["planId"] = "alternate-plan";
    alternate["rowWork"] = row;
    QFile alternateOutput(alternateFile);
    QVERIFY(alternateOutput.open(QIODevice::WriteOnly));
    QVERIFY(alternateOutput.write(QJsonDocument(alternate).toJson()) > 0);
    alternateOutput.close();

    auto *table = requireChild<QTableWidget>(ui.mapPage, "mapRowWorkCheckpointTable");
    table->item(0, 2)->setText("2500");
    QVERIFY(map.hasUnsavedMapChanges());
    const auto unsaved = RowWorkJson::planToJson(map.m_rowWorkPlan);
    auto *load = requireChild<QPushButton>(ui.mapPage, "mapLoadButton");
    auto *newMap = requireChild<QPushButton>(ui.mapPage, "mapNewButton");
    respondToMapDialogs([&] { load->click(); }, QMessageBox::Cancel, alternateFile);
    QCOMPARE(RowWorkJson::planToJson(map.m_rowWorkPlan), unsaved);
    QCOMPARE(map.m_currentMapFilePath, currentFile);
    respondToMapDialogs([&] { newMap->click(); }, QMessageBox::Cancel);
    QCOMPARE(RowWorkJson::planToJson(map.m_rowWorkPlan), unsaved);

    respondToMapDialogs([&] { load->click(); }, QMessageBox::Save, alternateFile);
    QCOMPARE(map.m_rowWorkPlan.planId, "alternate-plan");
    QCOMPARE(map.m_currentMapFilePath, alternateFile);
    QFile saved(currentFile);
    QVERIFY(saved.open(QIODevice::ReadOnly));
    const auto savedPlan = QJsonDocument::fromJson(saved.readAll()).object()["rowWork"].toObject();
    QCOMPARE(savedPlan, unsaved);
    saved.close();
    table->item(0, 2)->setText("3500");
    QVERIFY(map.hasUnsavedMapChanges());
    respondToMapDialogs([&] { load->click(); }, QMessageBox::Discard, currentFile);
    QCOMPARE(RowWorkJson::planToJson(map.m_rowWorkPlan), unsaved);
    QVERIFY(!map.hasUnsavedMapChanges());
    table->item(0, 2)->setText("4500");
    respondToMapDialogs([&] { newMap->click(); }, QMessageBox::Discard);
    QVERIFY(map.m_currentMapFilePath.isEmpty());
    QVERIFY(!map.m_rowWorkPlan.hasStartPose);
    QVERIFY(map.m_rowWorkPlan.checkpoints.isEmpty());
}

void MapRouteIntegrationTest::partialLineDraftIsDirtyAndSurvivesSaving()
{
    QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    for (int mode : {0, 1, 2}) {
        map.m_rowWorkPlan = {};
        map.syncCommittedMapState();
        map.m_rowWorkPlan.planId = QString("partial-%1").arg(mode);
        map.m_rowWorkPlan.hasStartPose = mode != 1;
        map.m_rowWorkPlan.hasEndPose = mode != 0;
        map.m_rowWorkPlan.startPose = map.m_rowWorkPlan.endPose = {1.23456789, 2.34567891, .123};
        map.refreshRowWorkUi();
        QVERIFY(!map.m_rowWorkPlan.isValid());
        QVERIFY(map.hasUnsavedMapChanges());
        const auto draft = RowWorkJson::planToJson(map.m_rowWorkPlan);
        QCOMPARE(map.serializeMap()["rowWork"].toObject(), draft);
        bool closeAccepted = true;
        respondToMapDialogs([&] { closeAccepted = map.confirmClose(); }, QMessageBox::Cancel);
        QVERIFY(!closeAccepted);
        QCOMPARE(RowWorkJson::planToJson(map.m_rowWorkPlan), draft);
        const QString filePath = directory.filePath(QString("partial-%1.json").arg(mode));
        QVERIFY(map.saveMapToFile(filePath));
        QFile saved(filePath);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        const auto object = QJsonDocument::fromJson(saved.readAll()).object();
        saved.close();
        QCOMPARE(object["rowWork"].toObject(), draft);
        map.m_rowWorkPlan = {};
        QVERIFY(map.deserializeMap(object));
        QCOMPARE(RowWorkJson::planToJson(map.m_rowWorkPlan), draft);
        QVERIFY(!map.m_rowWorkPlan.isValid());
    }
}

void MapRouteIntegrationTest::endpointFirstDraftCanBeReopenedAndCompleted()
{
    QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
    prepareStartFirstDraft(map);
    applyTaughtPose(map, Map::RowWorkPendingCaptureTarget::EndPose, {4, 5, .4});
    QVERIFY(!map.m_rowWorkPlan.hasStartPose);
    QVERIFY(map.m_rowWorkPlan.hasEndPose);
    QVERIFY(!map.m_rowWorkPlan.isValid());
    QVERIFY(map.m_rowWorkPlan.checkpoints.isEmpty());
    const auto draft = RowWorkJson::planToJson(map.m_rowWorkPlan);
    const auto document = map.serializeMap();
    map.m_rowWorkPlan = {};
    QVERIFY(map.deserializeMap(document));
    QCOMPARE(RowWorkJson::planToJson(map.m_rowWorkPlan), draft);
    applyTaughtPose(map, Map::RowWorkPendingCaptureTarget::StartPose, {0, 1, .4});
    QVERIFY(map.m_rowWorkPlan.isValid());
    QCOMPARE(map.m_rowWorkPlan.startPose.toPointF(), QPointF(0, 1));
    QCOMPARE(map.m_rowWorkPlan.endPose.toPointF(), QPointF(4, 5));
    QVERIFY(map.m_rowWorkPlan.checkpoints.isEmpty());
}

void MapRouteIntegrationTest::startFirstCheckpointsSurviveDraftSaveAndFinishOnTheFinalLine()
{
    QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
    prepareStartFirstDraft(map);
    auto *add = requireChild<QPushButton>(ui.mapPage, "mapRowWorkAddCheckpointFromMapButton");
    QVERIFY(!add->isEnabled());
    applyTaughtPose(map, Map::RowWorkPendingCaptureTarget::StartPose, {1, 2, .4});
    QVERIFY(map.m_rowWorkPlan.hasStartPose);
    QVERIFY(!map.m_rowWorkPlan.hasEndPose);
    QVERIFY(add->isEnabled());
    map.addRowWorkCheckpoint({2, 3});
    applyTaughtPose(map, Map::RowWorkPendingCaptureTarget::CheckpointFromVehicle, {4, 5, .6});
    QCOMPARE(map.m_rowWorkPlan.checkpoints.size(), 2);
    QVERIFY(map.m_rowWorkPlan.checkpoints[0].pendingMapPosition);
    QVERIFY(map.m_rowWorkPlan.checkpoints[1].pendingMapPosition);
    QCOMPARE(map.m_rowWorkCheckpointMarkers.size(), 2);
    QVERIFY(map.m_rowWorkStartMarker);
    QVERIFY(!map.m_rowWorkEndMarker);
    auto *table = requireChild<QTableWidget>(ui.mapPage, "mapRowWorkCheckpointTable");
    QVERIFY(!(table->item(0, 1)->flags() & Qt::ItemIsEditable));
    table->item(0, 2)->setText("1750");
    QCOMPARE(map.m_rowWorkPlan.checkpoints[0].dwellMs, 1750);
    table->item(0, 5)->setCheckState(Qt::Checked);
    QVERIFY(map.m_rowWorkPlan.checkpoints[0].capturePhoto);
    QVERIFY(map.hasUnsavedMapChanges());
    const auto draft = RowWorkJson::planToJson(map.m_rowWorkPlan);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto filePath = directory.filePath("start-first.json");
    QVERIFY(map.saveMapToFile(filePath));
    QFile saved(filePath);
    QVERIFY(saved.open(QIODevice::ReadOnly));
    const auto document = QJsonDocument::fromJson(saved.readAll()).object();
    saved.close();
    QCOMPARE(document["rowWork"].toObject(), draft);
    map.m_rowWorkPlan = {};
    QVERIFY(map.deserializeMap(document));
    QCOMPARE(RowWorkJson::planToJson(map.m_rowWorkPlan), draft);
    QVERIFY(!map.m_rowWorkPlan.isValid());
    QVERIFY(add->isEnabled());
    // B determines station distances while the original recorded coordinates remain.
    applyTaughtPose(map, Map::RowWorkPendingCaptureTarget::EndPose, {6, 7, -.2});
    QVERIFY(map.m_rowWorkPlan.isValid());
    QCOMPARE(map.m_rowWorkPlan.checkpoints.size(), 2);
    QVERIFY(!map.m_rowWorkPlan.checkpoints[0].pendingMapPosition);
    QVERIFY(std::abs(map.m_rowWorkPlan.checkpoints[0].progress - std::sqrt(2.)) < 1e-9);
    QVERIFY(std::abs(map.m_rowWorkPlan.checkpoints[1].progress - 3 * std::sqrt(2.)) < 1e-9);
    QCOMPARE(map.m_rowWorkPlan.checkpoints[0].dwellMs, 1750);
    QVERIFY(map.m_rowWorkPlan.checkpoints[0].capturePhoto);
    QVERIFY(!(table->item(0, 1)->flags() & Qt::ItemIsEditable));
    QCOMPARE(*map.m_rowWorkPlan.checkpoints[0].recordedMapPosition, QPointF(2, 3));
    QCOMPARE(*map.m_rowWorkPlan.checkpoints[1].recordedMapPosition, QPointF(4, 5));
}

void MapRouteIntegrationTest::draftEndpointAndStartChangesCannotSilentlyMoveCheckpoints()
{
    QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
    prepareStartFirstDraft(map);
    applyTaughtPose(map, Map::RowWorkPendingCaptureTarget::StartPose, {0, 0, 0});
    map.addRowWorkCheckpoint({1, .2});
    map.addRowWorkCheckpoint({4, .1});
    const auto original = RowWorkJson::planToJson(map.m_rowWorkPlan);
    for (const auto &end : {RowWorkPose{2, 0, 0}, RowWorkPose{-5, 0, 0}}) {
        applyTaughtPose(map, Map::RowWorkPendingCaptureTarget::EndPose, end);
        QCOMPARE(RowWorkJson::planToJson(map.m_rowWorkPlan), original);
        QVERIFY(!map.m_rowWorkPlan.hasEndPose);
        QVERIFY(map.m_routeStatusLabel->text().contains(QStringLiteral("中间点")));
    }
    applyTaughtPose(map, Map::RowWorkPendingCaptureTarget::StartPose, {.1, 0, 0});
    QCOMPARE(RowWorkJson::planToJson(map.m_rowWorkPlan), original);
    QVERIFY(map.m_routeStatusLabel->text().contains(QStringLiteral("起点")));
    applyTaughtPose(map, Map::RowWorkPendingCaptureTarget::EndPose, {5, 0, 0});
    QVERIFY(map.m_rowWorkPlan.isValid());
    QCOMPARE(map.m_rowWorkPlan.checkpoints[0].progress, 1.0);
    QCOMPARE(map.m_rowWorkPlan.checkpoints[1].progress, 4.0);
    QCOMPARE(*map.m_rowWorkPlan.checkpoints[0].recordedMapPosition, QPointF(1, .2));
    QCOMPARE(*map.m_rowWorkPlan.checkpoints[1].recordedMapPosition, QPointF(4, .1));
    const auto completed = RowWorkJson::planToJson(map.m_rowWorkPlan);
    applyTaughtPose(map, Map::RowWorkPendingCaptureTarget::EndPose, {3, 0, 0});
    QCOMPARE(RowWorkJson::planToJson(map.m_rowWorkPlan), completed);
    QVERIFY(map.m_routeStatusLabel->text().contains(QStringLiteral("中间点")));
}

void MapRouteIntegrationTest::editModeShowsSelectionAndCancelsPointPlacement()
{
    QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
    auto *mode = requireChild<QPushButton>(ui.mapPage, "mapEditModeButton");
    auto *placement = requireChild<QPushButton>(ui.mapPage, "mapAddPointFromClickButton");
    auto *points = requireChild<QTableWidget>(ui.mapPage, "mapPointTable");
    QVERIFY(mode->isCheckable());
    QVERIFY(mode->isChecked());
    QCOMPARE(mode->text(), QStringLiteral("编辑中"));
    placement->click();
    QVERIFY(placement->isChecked());

    mode->click();
    QVERIFY(!mode->isChecked());
    QCOMPARE(mode->text(), QStringLiteral("编辑模式"));
    QVERIFY(!placement->isChecked());
    QVERIFY(!placement->isEnabled());

    mode->click();
    QVERIFY(mode->isChecked());
    QCOMPARE(mode->text(), QStringLiteral("编辑中"));
    QVERIFY(placement->isEnabled());
    QVERIFY(!placement->isChecked());
    const int count = points->rowCount();
    QVERIFY(QMetaObject::invokeMethod(&map, "handleSceneClick", Qt::DirectConnection,
        Q_ARG(QPointF, QPointF(10, 10)), Q_ARG(Qt::MouseButton, Qt::LeftButton),
        Q_ARG(Qt::KeyboardModifiers, Qt::NoModifier)));
    QCOMPARE(points->rowCount(), count);
}

void MapRouteIntegrationTest::editModeSelectionSurvivesDisabledPage()
{
    QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
    auto *mode = requireChild<QPushButton>(ui.mapPage, "mapEditModeButton");
    QSignalSpy changes(mode, &QPushButton::toggled);
    ui.mapPage->setEnabled(false);
    mode->click();
    QVERIFY(mode->isChecked());
    QCOMPARE(mode->text(), QStringLiteral("编辑中"));
    QCOMPARE(changes.count(), 0);

    ui.mapPage->setEnabled(true);
    QVERIFY(mode->isEnabled());
    QVERIFY(mode->isChecked());
    mode->click();
    QVERIFY(!mode->isChecked());
    mode->click();
    QVERIFY(mode->isChecked());
    QCOMPARE(mode->text(), QStringLiteral("编辑中"));
    QCOMPARE(changes.count(), 2);
}

void MapRouteIntegrationTest::removesRetiredMissionEditorsAndNestedModeTabs()
{
    QMainWindow window;
    Ui::MainWindow ui;
    ui.setupUi(&window);

    Map map(&ui);
    QWidget *mapPage = ui.mapPage;
    QVERIFY(mapPage != nullptr);

    auto *taskTabs = requireChild<QTabWidget>(mapPage, "mapTaskTabWidget");
    QVERIFY(taskTabs != nullptr);
    taskTabs->setCurrentIndex(1);
    pumpEvents();

    QVERIFY(!mapPage->findChild<QTabWidget *>("mapRowWorkModeTabWidget"));
    QVERIFY(!mapPage->findChild<QWidget *>("mapRowWorkMissionPage"));
    QVERIFY(!mapPage->findChild<QWidget *>("mapRowMissionStepTable"));
    QVERIFY(!mapPage->findChild<QWidget *>("mapRowMissionEditorBox"));
    QVERIFY(requireChild<QScrollArea>(mapPage, "mapRowWorkPrimitiveScrollArea"));
    QVERIFY(requireChild<QPushButton>(mapPage, "mapRowWorkCaptureStartButton"));
    QVERIFY(requireChild<QPushButton>(mapPage, "mapRowWorkCaptureEndButton"));
}

void MapRouteIntegrationTest::separatesPointPathAndMapEditors()
{
    QMainWindow window;
    Ui::MainWindow ui;
    ui.setupUi(&window);

    Map map(&ui);
    QWidget *mapPage = ui.mapPage;
    QVERIFY(mapPage != nullptr);

    auto *editors = requireChild<QTabWidget>(mapPage, "mapEditorTabs");
    QVERIFY(editors);
    QCOMPARE(editors->count(), 3);
    const QStringList titles{QStringLiteral("点位"), QStringLiteral("路径"), QStringLiteral("地图")};
    const QStringList scrollNames{QStringLiteral("mapLeftScrollArea"),
        QStringLiteral("mapPathScrollArea"), QStringLiteral("mapSettingsScrollArea")};
    const QList<QStringList> groups{{QStringLiteral("mapPointBox"), QStringLiteral("mapBatchBox")},
        {QStringLiteral("mapPathBox")}, {QStringLiteral("mapGridBox")}};
    for (int i = 0; i < editors->count(); ++i) {
        QCOMPARE(editors->tabText(i), titles[i]);
        auto *scroll = editors->widget(i)->findChild<QScrollArea *>(scrollNames[i]);
        if (!scroll) scroll = qobject_cast<QScrollArea *>(editors->widget(i));
        QVERIFY(scroll);
        QCOMPARE(scroll->objectName(), scrollNames[i]);
        QVERIFY(scroll->widgetResizable());
        for (int j = 0; j < groups.size(); ++j) for (const auto &name : groups[j]) {
            auto *group = mapPage->findChild<QGroupBox *>(name);
            QVERIFY(group);
            QCOMPARE(editors->widget(i)->isAncestorOf(group), i == j);
        }
        editors->setCurrentIndex(i);
        pumpEvents();
        QVERIFY(scroll->isVisibleTo(editors));
    }
}



void MapRouteIntegrationTest::autoConnectsNewInputClickAndBatchPointsInOneDirection()
{
    QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
    auto *automatic = requireChild<QCheckBox>(ui.mapPage, "mapAutoConnectPointsCheck");
    QCOMPARE(map.m_pointDwellSpin->value(), 2000);
    QVERIFY(!automatic->isChecked());
    automatic->setChecked(true);
    addPoint(ui.mapPage, 0, 0);
    QCOMPARE(map.m_points.size(), 1);
    QVERIFY(map.m_paths.isEmpty());

    requireChild<QPushButton>(ui.mapPage, "mapAddPointFromClickButton")->click();
    map.handleSceneClick(map.mapToScene({1, 0}), Qt::LeftButton, Qt::NoModifier);
    QCOMPARE(map.m_points.size(), 2);
    QCOMPARE(map.m_paths.size(), 1);
    QVERIFY(map.findPathId(1, 2) > 0);
    QVERIFY(map.findRoutePathIds(2, 1).isEmpty());

    setComboToPointId(map.m_batchBasePointCombo, 2);
    map.m_batchDirectionCombo->setCurrentIndex(0);
    map.m_batchSpacingSpin->setValue(1);
    map.m_batchCountSpin->setValue(2);
    map.handleBatchGeneratePoints();
    QCOMPARE(map.m_points.size(), 4);
    QCOMPARE(map.m_paths.size(), 3);
    for (const auto &point : std::as_const(map.m_points)) QCOMPARE(point.dwellMs, 2000);
    for (int id = 1; id < 4; ++id) {
        const auto *path = map.pathById(map.findPathId(id, id + 1));
        QVERIFY(path);
        QVERIFY(!path->bidirectional);
        QVERIFY(path->type == Map::PathType::Line);
    }
    QCOMPARE(map.findRoutePathIds(1, 4).size(), 3);
    QVERIFY(map.findRoutePathIds(4, 1).isEmpty());
    automatic->setChecked(false);
    addPoint(ui.mapPage, 4, 0);
    QCOMPARE(map.m_paths.size(), 3);
}

void MapRouteIntegrationTest::autoConnectionDoesNotBackfillLoadOrMakeZeroLengthPaths()
{
    QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
    addPoint(ui.mapPage, 0, 0);
    addPoint(ui.mapPage, 1, 0);
    auto *automatic = requireChild<QCheckBox>(ui.mapPage, "mapAutoConnectPointsCheck");
    automatic->setChecked(true);
    QVERIFY(map.m_paths.isEmpty());
    addPoint(ui.mapPage, 2, 0);
    QCOMPARE(map.m_paths.size(), 1);
    QVERIFY(map.findPathId(1, 2) == -1);
    QVERIFY(map.findPathId(2, 3) > 0);
    auto saved = map.serializeMap();
    saved["paths"] = QJsonArray{};
    QVERIFY(map.deserializeMap(saved));
    QVERIFY(map.m_paths.isEmpty()); // Reading with the switch on does not synthesize edges.
    QVERIFY(map.addPointInternal(7, 0, 0, 7));
    QVERIFY(map.m_paths.isEmpty());
    addPoint(ui.mapPage, 8, 0);
    QVERIFY(map.findPathId(7, 8) > 0); // Gaps in old numbering use the existing predecessor.
    QCOMPARE(map.m_paths.size(), 1);
    const auto beforeRejectedPoint = map.buildComparableMapState();
    const int nextPointId = map.m_nextPointId;
    map.handleAddPointFromClickRequested();
    map.handleSceneClick(map.mapToScene({8, 0}), Qt::LeftButton, Qt::NoModifier);
    QCOMPARE(map.buildComparableMapState(), beforeRejectedPoint);
    QCOMPARE(map.m_nextPointId, nextPointId);
    QVERIFY(map.m_routeStatusLabel->text().contains(QStringLiteral("重合")));
    QVERIFY(map.m_waitingForClickPlacement); // A valid replacement click can complete this addition.
    map.handleSceneClick(map.mapToScene({9, 0}), Qt::LeftButton, Qt::NoModifier);
    QCOMPARE(map.m_points.size(), 6);
    QCOMPARE(map.m_paths.size(), 2);
    QVERIFY(map.pointById(nextPointId));
    QVERIFY(map.findPathId(8, nextPointId) > 0);
    QVERIFY(!map.m_waitingForClickPlacement);
}

void MapRouteIntegrationTest::routePointDwellEditingPreservesPrecisionAndMapRoundTrip()
{
    QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
    const QPointF precise(1.234567891234, 2.345678912345);
    const double yaw = .123456789123;
    QVERIFY(map.addPointInternal(0, 0, 0, 1, 2));
    QVERIFY(map.addPointInternal(precise.x(), precise.y(), yaw, 2, 1750));
    QVERIFY(map.addLinePath(1, 2));
    map.refreshPointUi();
    map.refreshSelectors();
    auto *table = requireChild<QTableWidget>(ui.mapPage, "mapPointTable");
    auto *dwell = requireChild<QSpinBox>(ui.mapPage, "mapPointDwellSpin");
    QCOMPARE(table->columnCount(), 5);
    QCOMPARE(dwell->minimum(), 0);
    QCOMPARE(dwell->maximum(), 3600000);
    map.m_pendingPointSelection = 2;
    map.refreshPointUi(); // A dwell value matching point ID 2 must not select point 1.
    QCOMPARE(table->currentRow(), 1);
    map.handlePointSelectionChanged();
    QCOMPARE(dwell->value(), 1750);
    map.refreshPlanningRevision();
    const auto revision = map.m_binding.context.mapRevision;
    dwell->setValue(3600000);
    map.handleUpdatePoint();
    QCOMPARE(map.pointById(2)->mapPosition, precise);
    QCOMPARE(map.pointById(2)->theta, yaw);
    QCOMPARE(map.pointById(2)->dwellMs, 3600000);
    QCOMPARE(map.m_binding.context.mapRevision, revision + 1);
    QCOMPARE(table->item(1, 4)->text(), "3600000");
    const auto saved = map.serializeMap();
    QCOMPARE(saved["schemaVersion"].toInt(), 4);
    map.m_autoConnectPointsCheck->setChecked(true);
    QVERIFY(map.deserializeMap(saved));
    QCOMPARE(map.m_paths.size(), 1);
    QCOMPARE(map.pointById(2)->mapPosition, precise);
    QCOMPARE(map.pointById(2)->theta, yaw);
    QCOMPARE(map.pointById(2)->dwellMs, 3600000);

    auto legacy = saved;
    auto points = legacy["points"].toArray();
    for (qsizetype i = 0; i < points.size(); ++i) {
        auto point = points[i].toObject();
        point.remove("dwellMs");
        points[i] = point;
    }
    legacy["points"] = points;
    legacy["schemaVersion"] = 3;
    QVERIFY(map.deserializeMap(legacy));
    QCOMPARE(map.pointById(1)->dwellMs, 0);
    QCOMPARE(map.pointById(2)->dwellMs, 0);
    QCOMPARE(map.serializeMap()["schemaVersion"].toInt(), 3);
    QCOMPARE(dwell->value(), 2000); // Loading old points does not change the new-point default.
    table->setCurrentCell(0, 0);
    QCOMPARE(dwell->value(), 0); // Selecting an old point still shows its saved value.
    map.resetToBlankMap();
    QCOMPARE(dwell->value(), 2000);
}

void MapRouteIntegrationTest::clickingNearRecordedPointOnlyChangesItsDwell()
{
    QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
    prepareStartFirstDraft(map);
    const RowWorkPose sampled{1.234567891234, 2.345678912345, .123456789123};
    map.m_pendingMapPointDwellMs = 2000;
    applyTaughtPose(map, Map::RowWorkPendingCaptureTarget::MapPoint, sampled);
    QCOMPARE(map.m_points.size(), 1);
    const auto position = map.pointById(1)->mapPosition;
    const auto yaw = map.pointById(1)->theta;
    map.handleSceneClick(map.mapToScene(position + QPointF(.12, -.08)), Qt::LeftButton, Qt::NoModifier);
    QCOMPARE(map.m_pendingPointSelection.value(), 1);
    map.m_pointDwellSpin->setValue(3000);
    map.handleUpdatePoint();
    QCOMPARE(map.pointById(1)->mapPosition, position);
    QCOMPARE(map.pointById(1)->theta, yaw);
    QCOMPARE(map.pointById(1)->dwellMs, 3000);
}

void MapRouteIntegrationTest::switchingCurrentSelectedPointRefreshesItsEditors()
{
    QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
    const QPointF first(1.234567891234, 2.345678912345);
    const QPointF second(4.567891234567, 5.678912345678);
    QVERIFY(map.addPointInternal(first.x(), first.y(), .123456789123, 1, 1000));
    QVERIFY(map.addPointInternal(second.x(), second.y(), .456789123456, 2, 3000));
    map.refreshPointUi();
    auto *table = map.m_pointTable;
    table->setCurrentCell(0, 0, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    table->setCurrentCell(1, 0, QItemSelectionModel::Select | QItemSelectionModel::Rows);
    QCOMPARE(table->selectionModel()->selectedRows().size(), 2);
    QCOMPARE(map.m_pointDwellSpin->value(), 3000);
    QSignalSpy selectionChanges(table, &QTableWidget::itemSelectionChanged);
    table->setCurrentCell(0, 0, QItemSelectionModel::NoUpdate);
    QCOMPARE(selectionChanges.size(), 0); // Current movement is distinct from a selection change.
    QCOMPARE(map.m_pendingPointSelection.value(), 1);
    QCOMPARE(map.m_pointDwellSpin->value(), 1000);
    map.m_pointDwellSpin->setValue(2500);
    map.handleUpdatePoint();
    QCOMPARE(map.pointById(1)->dwellMs, 2500);
    QCOMPARE(map.pointById(2)->dwellMs, 3000);
    QCOMPARE(map.pointById(1)->mapPosition, first);
    QCOMPARE(map.pointById(2)->mapPosition, second);
    QCOMPARE(map.pointById(1)->theta, .123456789123);
    QCOMPARE(map.pointById(2)->theta, .456789123456);
}

void MapRouteIntegrationTest::loadingMapAndReusingPointIdDoNotReuseOldSelection()
{
    QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
    QVERIFY(map.addPointInternal(1.123456789, 2.234567891, .123456789123, 1, 1000));
    map.refreshPointUi();
    auto *table = map.m_pointTable;
    table->setCurrentCell(0, 0);
    QCOMPARE(map.m_pendingPointSelection.value(), 1);
    QCOMPARE(map.m_pointDwellSpin->value(), 1000);

    const QPointF replacement(20.345678912345, 30.456789123456);
    const double replacementYaw = .734567891234;
    auto replacementMap = map.serializeMap();
    auto points = replacementMap["points"].toArray();
    auto point = points[0].toObject();
    point["x"] = replacement.x();
    point["y"] = replacement.y();
    point["theta"] = replacementYaw;
    point["dwellMs"] = 4500;
    points[0] = point;
    replacementMap["points"] = points;
    QVERIFY(map.deserializeMap(replacementMap));
    QVERIFY(!map.m_pendingPointSelection.has_value());
    QCOMPARE(table->currentRow(), -1);
    QVERIFY(table->selectedItems().isEmpty());
    QCOMPARE(map.m_pointDwellSpin->value(), 2000);
    QCOMPARE(map.pointById(1)->mapPosition, replacement);
    QCOMPARE(map.pointById(1)->theta, replacementYaw);
    table->setCurrentCell(0, 0);
    QCOMPARE(map.m_pointDwellSpin->value(), 4500);
    map.m_pointDwellSpin->setValue(2500);
    map.handleUpdatePoint();
    QCOMPARE(map.pointById(1)->mapPosition, replacement);
    QCOMPARE(map.pointById(1)->theta, replacementYaw);
    QCOMPARE(map.pointById(1)->dwellMs, 2500);

    map.removePointInternal(1);
    QVERIFY(!map.m_pendingPointSelection.has_value());
    QCOMPARE(table->currentRow(), -1);
    QVERIFY(table->selectedItems().isEmpty());
    const QPointF reused(-7.567891234567, -8.678912345678);
    QVERIFY(map.addPointInternal(reused.x(), reused.y(), -.345678912345, 1, 6000));
    map.refreshPointUi();
    QVERIFY(!map.m_pendingPointSelection.has_value());
    QCOMPARE(table->currentRow(), -1);
    QVERIFY(table->selectedItems().isEmpty());
    table->setCurrentCell(0, 0);
    QCOMPARE(map.m_pointDwellSpin->value(), 6000);
    map.m_pointDwellSpin->setValue(1500);
    map.handleUpdatePoint();
    QCOMPARE(map.pointById(1)->mapPosition, reused);
    QCOMPARE(map.pointById(1)->theta, -.345678912345);
    QCOMPARE(map.pointById(1)->dwellMs, 1500);
}

void MapRouteIntegrationTest::exhaustedMapIdsRejectNewObjectsWithoutChangingMap()
{
    QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
    QVERIFY(map.addPointInternal(0, 0, 0, 1, 2000));
    QVERIFY(map.addPointInternal(2, 0, 0, 2, 2000));
    QVERIFY(map.addLinePath(1, 2));
    auto document = map.serializeMap();
    const int highest = std::numeric_limits<int>::max() - 1;
    auto points = document["points"].toArray();
    bool replaced = false;
    for (qsizetype i = 0; i < points.size(); ++i) {
        auto point = points[i].toObject();
        if (point["id"].toInt() != 2) continue;
        point["id"] = highest;
        points[i] = point;
        replaced = true;
    }
    QVERIFY(replaced);
    document["points"] = points;
    auto paths = document["paths"].toArray();
    auto path = paths[0].toObject();
    path["id"] = highest;
    path["end"] = highest;
    paths[0] = path;
    document["paths"] = paths;
    QVERIFY(map.deserializeMap(document));
    const auto before = map.serializeMap();
    const auto beforeComparable = map.buildComparableMapState();
    QCOMPARE(map.m_nextPointId, std::numeric_limits<int>::max());
    QCOMPARE(map.m_nextPathId, std::numeric_limits<int>::max());
    QVERIFY(!map.addPointInternal(4, 0, 0));
    QVERIFY(!map.addPointInternal(4, 0, 0, std::numeric_limits<int>::max()));
    QVERIFY(!map.addLinePath(highest, 1));
    QVERIFY(!map.addLinePath(highest, 1, std::numeric_limits<int>::max()));
    QCOMPARE(map.m_nextPointId, std::numeric_limits<int>::max());
    QCOMPARE(map.m_nextPathId, std::numeric_limits<int>::max());
    QCOMPARE(map.serializeMap(), before);
    QCOMPARE(map.buildComparableMapState(), beforeComparable);
}

void MapRouteIntegrationTest::autoConnectionRejectsExhaustedPathIdsWithoutChangingMap()
{
    QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
    QVERIFY(map.addPointInternal(0, 0, 0, 1, 2000));
    QVERIFY(map.addPointInternal(2, 0, 0, 2, 3500));
    QVERIFY(map.addLinePath(1, 2));
    auto document = map.serializeMap();
    auto paths = document["paths"].toArray();
    auto path = paths.first().toObject();
    path["id"] = std::numeric_limits<int>::max() - 1;
    paths[0] = path;
    document["paths"] = paths;
    QVERIFY(map.deserializeMap(document));
    map.m_autoConnectPointsCheck->setChecked(true);
    map.m_pointTable->setCurrentCell(1, 0);
    const auto before = map.serializeMap();
    const auto beforeComparable = map.buildComparableMapState();
    const auto revision = map.m_binding.context.mapRevision;
    const auto selected = map.m_pendingPointSelection;
    const int currentRow = map.m_pointTable->currentRow();
    QCOMPARE(map.m_nextPointId, 3);
    QCOMPARE(map.m_nextPathId, std::numeric_limits<int>::max());

    QVERIFY(!map.addPointInternal(4, 0, 0));
    QCOMPARE(map.serializeMap(), before);
    QCOMPARE(map.buildComparableMapState(), beforeComparable);
    QCOMPARE(map.m_binding.context.mapRevision, revision);
    QCOMPARE(map.m_nextPointId, 3);
    QCOMPARE(map.m_nextPathId, std::numeric_limits<int>::max());
    QCOMPARE(map.m_pendingPointSelection.value(), selected.value());
    QCOMPARE(map.m_pointTable->currentRow(), currentRow);
    QCOMPARE(map.m_pointDwellSpin->value(), 3500);
    QVERIFY(map.m_routeStatusLabel->text().contains(QStringLiteral("路径编号")));

    map.handleAddPointFromClickRequested();
    map.handleSceneClick(map.mapToScene({4, 0}), Qt::LeftButton, Qt::NoModifier);
    QVERIFY(map.m_waitingForClickPlacement);
    QCOMPARE(map.serializeMap(), before);
    QCOMPARE(map.buildComparableMapState(), beforeComparable);
    QCOMPARE(map.m_nextPointId, 3);
    QVERIFY(map.m_routeStatusLabel->text().contains(QStringLiteral("路径编号")));

    map.m_autoConnectPointsCheck->setChecked(false);
    QVERIFY(map.addPointInternal(4, 0, 0));
    QCOMPARE(map.m_nextPointId, 4);
    QCOMPARE(map.m_paths.size(), 1);
    QVERIFY(map.pointById(3));
}

void MapRouteIntegrationTest::invalidMapLoadPreservesCurrentMapAndEditors_data()
{
    QTest::addColumn<QString>("invalid");
    for (const auto *invalid : {"duplicate-point-id", "duplicate-path-id", "dangling-path", "self-loop"})
        QTest::newRow(invalid) << QString::fromLatin1(invalid);
}

void MapRouteIntegrationTest::invalidMapLoadPreservesCurrentMapAndEditors()
{
    QFETCH(QString, invalid);
    QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
    QVERIFY(map.addPointInternal(0, 0, .1, 1, 2000));
    QVERIFY(map.addPointInternal(2.123456789, 0, .2, 2, 3500));
    QVERIFY(map.addLinePath(1, 2));
    map.refreshPointUi(); map.refreshSelectors();
    addRoute(ui.mapPage, 1, 2);
    map.m_pointTable->setCurrentCell(1, 0);
    QCOMPARE(map.m_pointDwellSpin->value(), 3500);
    const auto before = map.serializeMap();
    const auto beforeComparable = map.buildComparableMapState();
    const auto selected = map.m_pendingPointSelection;
    const int currentRow = map.m_pointTable->currentRow();
    const int queuedRoutes = map.m_routeQueue.size();
    auto corrupt = before;
    if (invalid == "duplicate-point-id") {
        auto points = corrupt["points"].toArray();
        auto duplicate = points.first().toObject();
        duplicate["dwellMs"] = 9000;
        points.append(duplicate);
        corrupt["points"] = points;
    } else {
        auto paths = corrupt["paths"].toArray();
        auto path = paths.first().toObject();
        if (invalid == "duplicate-path-id") paths.append(path);
        else {
            path["end"] = invalid == "self-loop" ? 1 : 999;
            paths[0] = path;
        }
        corrupt["paths"] = paths;
    }
    QVERIFY(!map.deserializeMap(corrupt));
    QCOMPARE(map.serializeMap(), before);
    QCOMPARE(map.buildComparableMapState(), beforeComparable);
    QCOMPARE(map.m_pendingPointSelection.value(), selected.value());
    QCOMPARE(map.m_pointTable->currentRow(), currentRow);
    QCOMPARE(map.m_routeQueue.size(), queuedRoutes);
    QCOMPARE(map.m_pointDwellSpin->value(), 3500);
}

void MapRouteIntegrationTest::routeSectionsIncludeIntermediateAndStartDwellOnce()
{
    QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
    for (int id = 1; id <= 4; ++id)
        QVERIFY(map.addPointInternal(id - 1, 0, .1 * id, id, id == 3 ? 0 : 1000 * id));
    QVERIFY(map.addLinePath(1, 2, std::nullopt, true));
    QVERIFY(map.addArcPath(2, 3, .1, std::nullopt, true));
    QVERIFY(map.addLinePath(3, 4, std::nullopt, true));
    map.refreshSelectors();
    addRoute(ui.mapPage, 1, 4);
    addRoute(ui.mapPage, 4, 1);
    QString error;
    auto sections = map.buildRouteSections(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(sections.size(), 4);
    QCOMPARE(sections[0].points, QList<QPointF>({{0, 0}, {1, 0}}));
    QCOMPARE(sections[0].dwellMs, 2000);
    QCOMPARE(sections[0].startDwellMs, 1000);
    QCOMPARE(*sections[0].startMapYaw, .1);
    QVERIFY(sections[1].points.size() > 3); // The intermediate non-dwell point and arc keep their geometry.
    QVERIFY(polylineContains(sections[1].points, {2, 0}));
    QCOMPARE(sections[1].points.back(), QPointF(3, 0));
    QCOMPARE(sections[1].dwellMs, 4000);
    QCOMPARE(sections[2].dwellMs, 2000);
    QCOMPARE(sections[3].dwellMs, 1000);
    for (qsizetype i = 1; i < sections.size(); ++i) QCOMPARE(sections[i].startDwellMs, 0);
    QCOMPARE(*sections[1].goalMapYaw, .4);
    QCOMPARE(*sections[2].goalMapYaw, .2);
    map.m_routeLoopCheck->setChecked(true);
    sections = map.buildRouteSections(&error);
    QCOMPARE(sections[0].startDwellMs, 0);
    QCOMPARE(sections.back().dwellMs, 1000); // A's dwell occurs only on return in each lap.
    map.m_routeLoopCheck->setChecked(false);
    map.pointById(1)->dwellMs = map.pointById(2)->dwellMs = map.pointById(4)->dwellMs = 0;
    sections = map.buildRouteSections(&error);
    QVERIFY(error.isEmpty());
    QCOMPARE(sections.size(), 2); // Zero values retain each original queue section.
    QCOMPARE(sections[0].points, map.composePolyline(map.m_routeQueue[0].pathIds, 1));
}

void MapRouteIntegrationTest::rightClickMenuRechecksTaskLockAfterNestedEvents_data()
{
    QTest::addColumn<QString>("state");
    QTest::addColumn<bool>("createPoint");
    for (const auto *state : {"Tracking", "Paused", "Completed"}) {
        QTest::newRow((QByteArray(state) + "-create").constData()) << QString::fromLatin1(state) << true;
        QTest::newRow((QByteArray(state) + "-delete").constData()) << QString::fromLatin1(state) << false;
    }
}

void MapRouteIntegrationTest::rightClickMenuRechecksTaskLockAfterNestedEvents()
{
    QFETCH(QString, state);
    QFETCH(bool, createPoint);
    MapMonitorFixture server;
    server.state = "Completed";
    QVERIFY(server.isListening());
    TrackingClient tracking; PoseClient pose; ControlSessionCoordinator coordinator(&tracking, &pose);
    QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
    map.setControlCoordinator(&coordinator);
    QVERIFY(map.addPointInternal(0, 0, 0, 1));
    QVERIFY(map.addPointInternal(1, 0, 0, 2));
    QVERIFY(map.addLinePath(1, 2));
    map.refreshPointUi(); map.refreshSelectors();
    tracking.configure(server.url(), {}, 500);
    tracking.start();
    QTRY_VERIFY(tracking.commandFresh());
    QCOMPARE(tracking.snapshot().state, QString("Completed"));
    QVERIFY(map.canEditRowWorkPlan());
    const auto before = map.buildComparableMapState();
    const auto pointCount = map.m_points.size();
    bool transitionRequested = false, actionTriggered = false, timedOut = false;
    QString chosenText;
    QElapsedTimer elapsed;
    elapsed.start();
    QTimer responder;
    connect(&responder, &QTimer::timeout, this, [&] {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if (!menu) {
            for (auto *candidate : ui.mapPage->findChildren<QMenu *>())
                if (candidate->isVisible()) { menu = candidate; break; }
        }
        if (elapsed.elapsed() > 3000) {
            timedOut = true;
            if (menu) menu->close();
            return;
        }
        if (!menu) return;
        if (!transitionRequested) {
            transitionRequested = true;
            server.state = state; // Delivered while QMenu::exec runs its nested event loop.
        }
        if (tracking.snapshot().state != state || menu->actions().isEmpty()) return;
        responder.stop();
        auto *action = menu->actions().first();
        chosenText = action->text();
        connect(action, &QAction::triggered, this, [&] { actionTriggered = true; });
        QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier, menu->actionGeometry(action).center());
    });
    responder.start(5);
    map.handleSceneClick(map.mapToScene(createPoint ? QPointF(3, 0) : QPointF(0, 0)),
                         Qt::RightButton, Qt::NoModifier);
    responder.stop();
    QVERIFY(!timedOut);
    QVERIFY(transitionRequested);
    QVERIFY(actionTriggered);
    QCOMPARE(chosenText, createPoint ? QStringLiteral("新建地图点") : QStringLiteral("删除地图点"));
    if (state == "Completed") {
        QCOMPARE(map.m_points.size(), pointCount + (createPoint ? 1 : -1));
        QVERIFY(map.buildComparableMapState() != before); // The simulated menu action actually executes.
    } else {
        QVERIFY(!map.canEditRowWorkPlan());
        QCOMPARE(map.buildComparableMapState(), before);
    }
    QCOMPARE(server.writes, 0);
    coordinator.shutdown(); tracking.stop();
}

QTEST_MAIN(MapRouteIntegrationTest)
#include "test_map_route_integration.moc"
