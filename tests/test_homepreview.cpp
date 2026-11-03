#include "camerapreviewselection.h"
#include "home.h"
#include "home_status_presenter.h"
#include "trackingtypes.h"
#include "configmanager.h"
#include "ui_mainwindow.h"

#include <QBuffer>
#include <QComboBox>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QStandardItemModel>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>
#include <QtMath>

class PreviewFixture : public QTcpServer {
public:
    QHash<QString,int> requests;
    QList<QPointer<QTcpSocket>> streams;
    int activeStreams=0, maximumStreams=0;
    bool depthAvailable=true, recording=false;
    PreviewFixture() {
        listen(QHostAddress::LocalHost);
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto *socket=nextPendingConnection();
                auto data=std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::disconnected, this, [this,socket] {
                    if (socket->property("preview-stream").toBool()) --activeStreams;
                });
                connect(socket, &QTcpSocket::readyRead, this, [this,socket,data] {
                    data->append(socket->readAll());
                    if (!data->contains("\r\n\r\n") || socket->property("handled").toBool()) return;
                    socket->setProperty("handled", true);
                    const QString path=QString::fromUtf8(data->split(' ').value(1));
                    ++requests[path];
                    if (path=="/camera/stream.mjpeg" || path=="/camera/depth/stream.mjpeg") {
                        if (path.contains("/depth/") && !depthAvailable) {
                            reply(socket, {{"message","depth unavailable"}},503); return;
                        }
                        socket->setProperty("preview-stream",true);
                        socket->setProperty("preview-depth",path.contains("/depth/"));
                        ++activeStreams; maximumStreams=qMax(maximumStreams,activeStreams);
                        streams.append(socket);
                        socket->write("HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=frame\r\nConnection: keep-alive\r\n\r\n");
                        sendFrame(socket);
                    } else if (path=="/camera/status") {
                        reply(socket,{{"success",true},{"cameraConnected",true},{"recording",recording}});
                    } else if (path=="/camera/photo") {
                        reply(socket,{{"success",true},{"path","/home/nvidia/tenco-project/oak-camera-service/Capture/rgb.jpg"}});
                    } else if (path=="/camera/record/start" || path=="/camera/record/stop") {
                        recording=path.endsWith("/start");
                        reply(socket,{{"success",true},{"recording",recording},{"path","/home/nvidia/tenco-project/oak-camera-service/Capture/rgb.mp4"}});
                    } else reply(socket,{},404);
                });
            }
        });
    }
    QUrl url() const { return QUrl(QString("http://127.0.0.1:%1").arg(serverPort())); }
    void sendFrame(QTcpSocket *socket) {
        QImage image(32,24,QImage::Format_RGB32);
        image.fill(socket->property("preview-depth").toBool() ? Qt::green : Qt::red);
        QByteArray jpeg; QBuffer buffer(&jpeg); buffer.open(QIODevice::WriteOnly); image.save(&buffer,"JPEG");
        socket->write("--frame\r\nContent-Type: image/jpeg\r\nContent-Length: "+QByteArray::number(jpeg.size())+"\r\n\r\n"+jpeg+"\r\n");
    }
    static void reply(QTcpSocket *socket,const QJsonObject &object,int status=200) {
        const auto bytes=QJsonDocument(object).toJson(QJsonDocument::Compact);
        socket->write("HTTP/1.1 "+QByteArray::number(status)+" Result\r\nContent-Type: application/json\r\nContent-Length: "+QByteArray::number(bytes.size())+"\r\nConnection: close\r\n\r\n"+bytes);
        socket->disconnectFromHost();
    }
};

class HomePreviewTest : public QObject {
    Q_OBJECT
    static void configure(QTemporaryDir &directory,const PreviewFixture &server,const QString &backend="mjpeg_http") {
        QFile file(directory.filePath("config.json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        const auto base=server.url().toString();
        file.write(QJsonDocument(QJsonObject{{"schemaVersion",2},
            {"video",QJsonObject{{"backend",backend},{"streamUrl",base+"/camera/stream.mjpeg"},{"controlBaseUrl",base},
                {"autoStart",false},{"reconnectIntervalMs",250},{"previewFps",30}}},
            {"network",QJsonObject{{"websocketUrl","ws://127.0.0.1:1"},{"statusReadUrl",base+"/chassis"},
                {"statusPollIntervalMs",5000},{"authToken",""},{"chassisAutoReconnect",false}}},
            {"gimbal",QJsonObject{{"enabled",false}}}}).toJson());
        file.close();
        auto &config=ConfigManager::instance();
        config.setConfigFilePath(file.fileName()); config.reload();
        QVERIFY(config.loadedFromFile());
    }
    static QColor shownColor(QLabel *display) {
        const auto pixmap=display->pixmap();
        return pixmap.isNull() ? QColor() : pixmap.toImage().pixelColor(0,0);
    }
private slots:
    void homeShowsSharedPoseAndTaskWithoutMixingChassisMetadata() {
        PreviewFixture server; QTemporaryDir directory; configure(directory,server);
        QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window);
        Home home(&ui);
        QVERIFY(ui.homeLocalizationState->isReadOnly());
        QVERIFY(ui.homeTaskState->isReadOnly());
        QVERIFY(ui.homeTaskProgress->isReadOnly());
        QVERIFY(!window.findChild<QWidget *>("comboBox_Mode"));
        QVERIFY(!window.findChild<QWidget *>("pushButton"));
        QVERIFY(ui.homeLocalizationState->text().contains("等待定位连接"));
        QVERIFY(ui.homeTaskState->text().contains("未连接"));

        ControlPoseSnapshot pose;
        pose.bootId="fusion-1"; pose.validForControl=true; pose.stateAgeMs=12;
        pose.reasons={"uncertainty_unknown"};
        home.setControlPoseStatus(pose);
        QCOMPARE(ui.homeLocalizationState->text(),QString("定位可用"));
        QVERIFY(ui.homeLocalizationState->toolTip().contains("误差未评估"));
        home.setPoseMonitoringAvailability(false,"无线位姿连接延迟");
        QVERIFY(ui.homeLocalizationState->text().contains("显示延迟"));
        QVERIFY(!ui.homeLocalizationState->text().contains("定位可用"));
        home.setControlPoseStatus(pose);
        QCOMPARE(ui.homeLocalizationState->text(),QString("定位可用"));
        pose.validForControl=false; pose.reasons={"initializing"};
        home.setControlPoseStatus(pose);
        QCOMPARE(ui.homeLocalizationState->text(),QString("融合初始化中"));
        pose.reasons={"nonfinite_estimate"};
        home.setControlPoseStatus(pose);
        QCOMPARE(ui.homeLocalizationState->text(),QString("定位不可用"));

        TrackingSnapshot task;
        task.bootId="tracking-1"; task.state="Tracking"; task.taskId="regular-1";
        task.stepId="path-2"; task.stepIndex=3; task.progressMeters=1.25; task.lengthMeters=4.5;
        task.loopIndex=1;
        home.setTaskStatus(task,true);
        QVERIFY(ui.homeTaskState->text().contains("执行中"));
        QVERIFY(ui.homeTaskProgress->text().contains("步骤 4"));
        QVERIFY(ui.homeTaskProgress->text().contains("本段 1.25/4.50 m"));
        QVERIFY(ui.homeTaskProgress->text().contains("第 2 轮"));
        QVERIFY(!ui.homeTaskProgress->text().contains('%'));
        const auto state=ui.homeTaskState->text(), progress=ui.homeTaskProgress->text();
        // Chassis register metadata and a chassis-only outage must never erase
        // the independent IPC task or localization summary.
        const auto localization=ui.homeLocalizationState->text();
        HomeStatusPresenter chassis(&ui, {});
        chassis.setControlPoseStatus(pose); chassis.setTaskStatus(task,true);
        chassis.handleStatusPacket({{"data",QJsonArray{
            QJsonObject{{"address",StatusProtocol::Address::kMapName},{"value",QJsonArray{"legacy-map"}}},
            QJsonObject{{"address",StatusProtocol::Address::kWorkMode},{"value",QJsonArray{2}}}
        }}});
        QCOMPARE(ui.homeTaskState->text(),state);
        QCOMPARE(ui.homeTaskProgress->text(),progress);
        chassis.handleNetworkFailure(0,"offline",{});
        QCOMPARE(ui.homeTaskState->text(),state);
        QCOMPARE(ui.homeTaskProgress->text(),progress);
        QCOMPARE(ui.homeLocalizationState->text(),localization);

        task.measuredValid=true; task.measuredStopped=true;
        home.setTaskStatus(task,false);
        QVERIFY(ui.homeTaskState->text().contains("显示延迟"));
        QVERIFY(ui.homeTaskProgress->text().startsWith("上次"));
        QVERIFY(!ui.homeTaskState->text().contains("已停稳"));
        QVERIFY(ui.homeTaskState->toolTip().contains("停稳状态未知"));
        home.setTaskStatus(task,true);
        QCOMPARE(ui.homeTaskState->text(),state);
        QCOMPARE(ui.homeTaskProgress->text(),progress);
        QTest::qWait(100);
        QCOMPARE(server.requests.value("/tracking/status"),0);
        QCOMPARE(server.requests.value("/pose/status"),0);
    }

    void taskSummaryDistinguishesTurnsWaitsPauseAndFault() {
        QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window);
        HomeStatusPresenter presenter(&ui, {});
        TrackingSnapshot task;
        task.bootId="tracking-1"; task.taskId="row-1"; task.state="Turning";
        task.remainingAngleRad=qDegreesToRadians(45.0);
        task.lengthMeters=4.0; task.progressMeters=2.0;
        presenter.setTaskStatus(task,true);
        QVERIFY(ui.homeTaskState->text().contains("转向中"));
        QVERIFY(ui.homeTaskProgress->text().contains("剩余 45.0°"));
        QVERIFY(!ui.homeTaskProgress->text().contains("本段"));
        task.turnRecovering=true; task.turnOffsetMeters=.08;
        presenter.setTaskStatus(task,true);
        QVERIFY(ui.homeTaskProgress->text().contains("回点偏移 0.08 m"));
        QVERIFY(!ui.homeTaskProgress->text().contains("剩余"));
        task.turnRecovering=false;
        task.state="Waiting"; task.remainingWaitMs=1500;
        presenter.setTaskStatus(task,true);
        QVERIFY(ui.homeTaskProgress->text().contains("等待 1.5 s"));
        task.waitingEventId="photo-1";
        presenter.setTaskStatus(task,true);
        QVERIFY(ui.homeTaskProgress->text().contains("等待检查点确认"));
        task.state="Paused"; task.pauseReason="goal_position_outside_tolerance";
        presenter.setTaskStatus(task,true);
        QVERIFY(ui.homeTaskState->text().contains("已暂停 · 到点超差"));
        QVERIFY(ui.homeTaskState->toolTip().contains("未准确到达目标点"));
        task.autoResumePending=true; task.pauseReason="control_cycle_late";
        presenter.setTaskStatus(task,true);
        QVERIFY(ui.homeTaskState->text().contains("控制调度延迟"));
        QVERIFY(ui.homeTaskState->text().contains("等待自动恢复"));
        task.returningToStart=true; task.pauseReason="pose_sample_jump";
        presenter.setTaskStatus(task,true);
        QVERIFY(ui.homeTaskState->text().contains("定位跳点"));
        QVERIFY(ui.homeTaskProgress->text().contains("回起点准备"));
        task.returningToStart=false;
        task.state="Fault"; task.faultCode="turn_timeout"; task.autoResumePending=false;
        presenter.setTaskStatus(task,true);
        QVERIFY(ui.homeTaskState->text().contains("故障停车 · 转向超时"));
        QCOMPARE(ui.homeTaskProgress->text(),QString("任务故障"));
        task.state="Completed"; task.faultCode.clear();
        presenter.setTaskStatus(task,true);
        QCOMPARE(ui.homeTaskProgress->text(),QString("任务已完成"));
        task.state="Aborted";
        presenter.setTaskStatus(task,true);
        QCOMPARE(ui.homeTaskProgress->text(),QString("任务已中止"));
        task.state="Ready";
        presenter.setTaskStatus(task,true);
        QCOMPARE(ui.homeTaskProgress->text(),QString("等待启动"));
        task={}; task.bootId="tracking-1"; task.state="Idle";
        presenter.setTaskStatus(task,true);
        QVERIFY(ui.homeTaskState->text().contains("待命"));
        QCOMPARE(ui.homeTaskProgress->text(),QString("暂无任务"));
        QVERIFY(!ui.homeTaskProgress->text().contains("步骤"));
    }

    void classicBatteryAndSpeedRemainVisibleAcrossFeedbackLoss() {
        QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window);
        HomeStatusPresenter presenter(&ui, {});
        QVERIFY(!ui.battery->isHidden());
        QCOMPARE(ui.lcdNumber->value(), 0.0);
        QVERIFY(ui.lcdNumber->toolTip().contains("初始值或上次反馈"));
        QVERIFY(ui.lineEdit_BattryVol->text().isEmpty());
        const QJsonObject packet{{"data", QJsonArray{
            QJsonObject{{"address",StatusProtocol::Address::kBatteryPercent},{"value",QJsonArray{63}}},
            QJsonObject{{"address",StatusProtocol::Address::kVelocity},{"value",QJsonArray{0.35}}},
            QJsonObject{{"address",StatusProtocol::Address::kBatteryVoltage},{"value",QJsonArray{24.5}}}
        }}};
        presenter.handleStatusPacket(packet);
        QVERIFY(!ui.battery->isHidden());
        QCOMPARE(ui.lable_battray->text(), QString("63%"));
        QCOMPARE(ui.lineEdit_BattryVol->text(), QString("24.50V"));
        QCOMPARE(ui.lcdNumber->value(), 0.35);
        QVERIFY(ui.battery->toolTip().contains("电量反馈"));
        presenter.handleNetworkFailure(0, "offline", {});
        QVERIFY(!ui.battery->isHidden());
        QCOMPARE(ui.lable_battray->text(), QString("63%"));
        QCOMPARE(ui.lcdNumber->value(), 0.35);
        QVERIFY(ui.lineEdit_BattryVol->text().isEmpty());
        QVERIFY(ui.lcdNumber->toolTip().contains("不能据此判断车辆已停稳"));
        presenter.handleStatusPacket(packet);
        QCOMPARE(ui.lineEdit_BattryVol->text(), QString("24.50V"));
        QVERIFY(!ui.battery->isHidden());
    }

    void streamUrlsUseExplicitCameraEndpoints() {
        auto choices=CameraPreviewSelection::streams("mjpeg_http","http://rgb.example:18080/custom/rgb.mjpeg?quality=90","https://camera.example:8443/base?old=1#old");
        QCOMPARE(choices.size(),2);
        QCOMPARE(choices[0].url,QUrl("https://camera.example:8443/camera/stream.mjpeg"));
        QCOMPARE(choices[1].url,QUrl("https://camera.example:8443/camera/depth/stream.mjpeg"));
        choices=CameraPreviewSelection::streams("mjpeg_http","http://camera.example:18080/camera/stream.mjpeg?old=1",{});
        QCOMPARE(choices[0].url,QUrl("http://camera.example:18080/camera/stream.mjpeg?old=1"));
        QCOMPARE(choices[1].url,QUrl("http://camera.example:18080/camera/depth/stream.mjpeg"));
        choices=CameraPreviewSelection::streams("mjpeg_http","http://camera.example:18080/camera/depth/stream.mjpeg","http://camera.example:18080");
        QCOMPARE(choices[0].url.path(),QString("/camera/stream.mjpeg"));
        choices=CameraPreviewSelection::streams("mjpeg_http","garbage","file:///tmp/source");
        QVERIFY(!choices[0].available); QVERIFY(!choices[1].available);
        choices=CameraPreviewSelection::streams("oak_depthai",{},{});
        QVERIFY(choices[0].available); QVERIFY(!choices[1].available); QVERIFY(!choices[1].reason.isEmpty());
    }
    void closedPreviewSelectionDoesNotOpenAStreamAndLocalDepthIsDisabled() {
        PreviewFixture server; QTemporaryDir directory; configure(directory,server);
        QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window);
        Home home(&ui); window.show();
        QVERIFY(!window.findChild<QWidget *>("savePathButton"));
        QVERIFY(!window.findChild<QWidget *>("video_topic_name"));
        QCOMPARE(ui.cameraPreviewKind->count(),2);
        ui.cameraPreviewKind->setCurrentIndex(1);
        QTest::qWait(150);
        QCOMPARE(server.requests.value("/camera/stream.mjpeg"),0);
        QCOMPARE(server.requests.value("/camera/depth/stream.mjpeg"),0);
        QVERIFY(ui.videoDisplay->text().contains("深度预览未开启"));
        configure(directory,server,"oak_depthai");
        auto *model=qobject_cast<QStandardItemModel *>(ui.cameraPreviewKind->model());
        QVERIFY(model); QVERIFY(!model->item(1)->isEnabled());
        QVERIFY(ui.cameraPreviewKind->itemData(1,Qt::ToolTipRole).toString().contains("本机 OAK"));
    }
    void switchingClearsOldImageCancelsOldStreamAndKeepsMediaRgb() {
        PreviewFixture server; QTemporaryDir directory; configure(directory,server);
        QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window);
        Home home(&ui); window.show();
        QTRY_VERIFY(ui.captureButton->isEnabled());
        ui.videoPreviewButton->click();
        QTRY_VERIFY(shownColor(ui.videoDisplay).red()>200);
        ui.cameraPreviewKind->setCurrentIndex(1);
        QVERIFY(ui.videoDisplay->pixmap().isNull());
        QTRY_VERIFY(shownColor(ui.videoDisplay).green()>200);
        QTRY_COMPARE(server.activeStreams,1);
        QCOMPARE(server.requests.value("/camera/stream.mjpeg"),1);
        QCOMPARE(server.requests.value("/camera/depth/stream.mjpeg"),1);
        // Photo/record are service RGB commands even with depth selected.
        auto *dismiss=new QTimer(&window); dismiss->setInterval(10);
        connect(dismiss,&QTimer::timeout,&window,[] {
            if (auto *box=qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) box->accept();
        }); dismiss->start();
        ui.captureButton->click(); QTRY_COMPARE(server.requests.value("/camera/photo"),1);
        QTRY_VERIFY(ui.captureButton->isEnabled()); QTest::qWait(50);
        ui.recordButton->click(); QTRY_COMPARE(server.requests.value("/camera/record/start"),1);
        QTRY_VERIFY(ui.recordButton->text().contains("停止"));
        ui.videoPreviewButton->click();
        QTRY_COMPARE(server.activeStreams,0);
        QVERIFY(ui.videoDisplay->pixmap().isNull());
        QVERIFY(server.recording); // Closing preview cannot stop remote RGB recording.
        const int requests=server.requests.value("/camera/depth/stream.mjpeg");
        QTest::qWait(650);
        QCOMPARE(server.requests.value("/camera/depth/stream.mjpeg"),requests);
        QCOMPARE(server.maximumStreams,1);
    }
    void unavailableDepthDoesNotFallBackToRgbAndSwitchCancelsReconnect() {
        PreviewFixture server; server.depthAvailable=false;
        QTemporaryDir directory; configure(directory,server);
        QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window);
        Home home(&ui); window.show();
        ui.videoPreviewButton->click();
        QTRY_VERIFY(shownColor(ui.videoDisplay).red()>200);
        ui.cameraPreviewKind->setCurrentIndex(1);
        QTRY_VERIFY(server.requests.value("/camera/depth/stream.mjpeg")>0);
        QTRY_VERIFY(ui.videoDisplay->text().contains("深度预览暂不可用"));
        QVERIFY(ui.videoDisplay->pixmap().isNull());
        QCOMPARE(server.requests.value("/camera/stream.mjpeg"),1);
        ui.cameraPreviewKind->setCurrentIndex(0);
        QTRY_VERIFY(shownColor(ui.videoDisplay).red()>200);
        const int failures=server.requests.value("/camera/depth/stream.mjpeg");
        QTest::qWait(650);
        QCOMPARE(server.requests.value("/camera/depth/stream.mjpeg"),failures);
    }
};

QTEST_MAIN(HomePreviewTest)
#include "test_homepreview.moc"
