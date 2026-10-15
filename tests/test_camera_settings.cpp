#include "camerasettingspanel.h"
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

class CameraFixture : public QTcpServer {
public:
    QJsonObject settings{{"sensorFps",30},
        {"preview",QJsonObject{{"width",960},{"height",540},{"fps",15}}},
        {"photo",QJsonObject{{"width",0},{"height",0},{"fps",2}}},
        {"recording",QJsonObject{{"width",1280},{"height",720},{"fps",30}}}};
    QString revision="1", state="applied", error;
    int writes=0, reads=0, pendingPolls=0;
    bool recording=false, reject=false, dropWriteReply=false, unsupported=false;
    QByteArray lastMethod;
    CameraFixture() {
        listen(QHostAddress::LocalHost);
        connect(this,&QTcpServer::newConnection,this,[this] {
            while(hasPendingConnections()) {
                auto *s=nextPendingConnection(); auto data=std::make_shared<QByteArray>();
                connect(s,&QTcpSocket::disconnected,s,&QObject::deleteLater);
                connect(s,&QTcpSocket::readyRead,this,[this,s,data] {
                    data->append(s->readAll()); const int end=data->indexOf("\r\n\r\n");
                    if(end<0||s->property("handled").toBool()) return;
                    const auto header=data->left(end); int length=0;
                    for(const auto &line:header.split('\n')) if(line.toLower().startsWith("content-length:")) length=line.mid(15).trimmed().toInt();
                    if(data->size()<end+4+length) return;
                    s->setProperty("handled",true);
                    const auto line=header.split('\n').first().split(' ');
                    const auto method=line.value(0),path=line.value(1);
                    const auto body=QJsonDocument::fromJson(data->mid(end+4,length)).object();
                    if(path=="/camera/capabilities") {
                        reply(s,{{"schemaVersion",1},{"sensorName","IMX378"},
                            {"native",QJsonObject{{"width",4056},{"height",3040},{"minFps",1.42},{"maxFps",30}}},
                            {"resolutions",QJsonArray{QJsonObject{{"width",640},{"height",480}},QJsonObject{{"width",960},{"height",540}},QJsonObject{{"width",1280},{"height",720}},QJsonObject{{"width",4056},{"height",3040}}}}},unsupported?404:200);
                    } else if(path=="/camera/settings"&&method=="GET") {
                        ++reads; if(pendingPolls>0&&!--pendingPolls) state="applied";
                        reply(s,configuration());
                    } else if(path=="/camera/settings") {
                        ++writes;lastMethod=method;
                        if(reject||recording||body["expectedRevision"]!=revision) {reply(s,{{"message","settings_revision_conflict"}},409);return;}
                        settings=body["settings"].toObject();revision=QString::number(revision.toInt()+1);state="pending";pendingPolls=2;
                        if(dropWriteReply) s->disconnectFromHost();
                        else reply(s,configuration(),202);
                    } else if(path=="/camera/status") reply(s,{{"cameraConnected",true},{"recording",recording},
                        {"recordingActualFps",24.5},{"recordingFrames",245},{"recordingDroppedFrames",0},{"mediaConfiguration",configuration()}});
                    else reply(s,{},404);
                });
            }
        });
    }
    QJsonObject configuration() const {
        return {{"schemaVersion",1},{"revision",revision},{"activeRevision",revision},{"applyState",state},{"lastError",error},
            {"cameraConnected",true},{"recording",recording},{"settings",settings},{"activeSettings",settings},{"persisted",true}};
    }
    QUrl url() const {return QUrl(QString("http://127.0.0.1:%1").arg(serverPort()));}
    static void reply(QTcpSocket *s,const QJsonObject &body,int code=200) {
        const auto bytes=QJsonDocument(body).toJson(QJsonDocument::Compact);
        s->write("HTTP/1.1 "+QByteArray::number(code)+" Result\r\nContent-Type: application/json\r\nContent-Length: "+QByteArray::number(bytes.size())+"\r\nConnection: close\r\n\r\n"+bytes);
        s->disconnectFromHost();
    }
};
class CameraSettingsTest : public QObject {
    Q_OBJECT
private slots:
    void explicitReadAndApplyFromPanel() {
        CameraFixture server;CameraSettingsPanel panel;panel.resize(900,650);
        panel.setConnectionProvider([&] {return CameraSettingsConnection{server.url(),{},500};});panel.show();
        auto *read=panel.findChild<QPushButton*>("cameraSettingsRead");
        auto *apply=panel.findChild<QPushButton*>("cameraSettingsApply");
        QVERIFY(!apply->isEnabled());QCOMPARE(server.writes,0);
        QTest::mouseClick(read,Qt::LeftButton);QTRY_VERIFY(apply->isEnabled());QCOMPARE(server.writes,0);
        const auto screenshot=qEnvironmentVariable("TENCO_CAMERA_PANEL_SCREENSHOT");
        if(!screenshot.isEmpty()) QVERIFY(panel.grab().save(screenshot));
        auto *fps=panel.findChild<QDoubleSpinBox*>("cameraFps_recording");fps->setValue(20);
        QTest::mouseClick(apply,Qt::LeftButton);QVERIFY(!apply->isEnabled());
        QTRY_VERIFY(panel.findChild<QLabel*>("cameraSettingsResult")->text().contains("已在工控机生效"));
        QCOMPARE(server.writes,1);QCOMPARE(server.lastMethod,QByteArray("PUT"));
        QCOMPARE(server.settings["recording"].toObject()["fps"].toDouble(),20.0);
        QTRY_VERIFY(panel.findChild<QLabel*>("cameraSettingsRuntime")->text().contains("24.50"));
        server.recording=true;QTRY_VERIFY(!apply->isEnabled());
        server.recording=false;QTRY_VERIFY(apply->isEnabled());
        server.revision="99";QTRY_VERIFY(panel.findChild<QLabel*>("cameraSettingsResult")->text().contains("参数已变化"));
        QVERIFY(!apply->isEnabled());
    }
    void conflictRequiresReread() {
        CameraFixture server;CameraSettingsClient client;client.configure(server.url(),{},500);
        QSignalSpy finished(&client,&CameraSettingsClient::finished);client.read();QTRY_COMPARE(finished.count(),1);
        server.reject=true;QVERIFY(client.apply(server.settings));QTRY_COMPARE(finished.count(),2);
        QVERIFY(!finished.last()[0].toBool());QVERIFY(client.configuration().isEmpty());
        QVERIFY(!client.apply(server.settings));QCOMPARE(server.writes,1);
    }
    void lostPutReplyIsResolvedWithoutReplay() {
        CameraFixture server;CameraSettingsClient client;client.configure(server.url(),{},500);
        QSignalSpy finished(&client,&CameraSettingsClient::finished);client.read();QTRY_COMPARE(finished.count(),1);
        server.dropWriteReply=true;auto settings=server.settings;settings["sensorFps"]=25;
        QVERIFY(client.apply(settings));QTRY_COMPARE_WITH_TIMEOUT(finished.count(),2,4000);
        QVERIFY(finished.last()[0].toBool());QCOMPARE(server.writes,1);QCOMPARE(client.configuration()["activeSettings"].toObject(),settings);
    }
    void rejectedApplicationIsReported() {
        CameraFixture server;CameraSettingsClient client;client.configure(server.url(),{},500);
        QSignalSpy finished(&client,&CameraSettingsClient::finished);client.read();QTRY_COMPARE(finished.count(),1);
        auto settings=server.settings;settings["sensorFps"]=20;QVERIFY(client.apply(settings));
        QTRY_COMPARE(server.writes,1);server.pendingPolls=0;server.state="rejected";server.error="device_failure";
        QTRY_COMPARE(finished.count(),2);QVERIFY(!finished.last()[0].toBool());QVERIFY(finished.last()[1].toString().contains("device_failure"));
    }
    void oldServiceAndCancellation() {
        CameraFixture server;server.unsupported=true;CameraSettingsClient client;client.configure(server.url(),{},500);
        QSignalSpy finished(&client,&CameraSettingsClient::finished);client.read();QTRY_COMPARE(finished.count(),1);
        QVERIFY(finished.last()[1].toString().contains("更新服务"));QVERIFY(client.configuration().isEmpty());
        server.unsupported=false;client.read();client.configure(QUrl(),{});QTest::qWait(200);
        QCOMPARE(finished.count(),1);QVERIFY(!client.busy());
    }
    void realIpcIntegration() {
        const auto url=qEnvironmentVariable("TENCO_CAMERA_PROBE_URL");
        if(url.isEmpty()) QSKIP("Set TENCO_CAMERA_PROBE_URL for an explicitly scheduled IPC integration run");
        CameraSettingsPanel panel;panel.setConnectionProvider([&] {return CameraSettingsConnection{QUrl(url),{},5000};});panel.show();
        auto *read=panel.findChild<QPushButton*>("cameraSettingsRead");auto *apply=panel.findChild<QPushButton*>("cameraSettingsApply");
        QTest::mouseClick(read,Qt::LeftButton);QTRY_VERIFY_WITH_TIMEOUT(apply->isEnabled(),10000);
        auto *fps=panel.findChild<QDoubleSpinBox*>("cameraFps_recording");const double original=fps->value();
        const double target=original==20?25:20;fps->setValue(target);
        QTest::mouseClick(apply,Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(panel.findChild<QLabel*>("cameraSettingsResult")->text().contains("已在工控机生效"),40000);
        QCOMPARE(fps->value(),target);QVERIFY(apply->isEnabled());
        fps->setValue(original);QTest::mouseClick(apply,Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(panel.findChild<QLabel*>("cameraSettingsResult")->text().contains("已在工控机生效"),40000);
        QCOMPARE(fps->value(),original);
        qInfo()<<"Real IPC panel read, apply, confirmed readback and restore passed";
    }
};
QTEST_MAIN(CameraSettingsTest)
#include "test_camera_settings.moc"
