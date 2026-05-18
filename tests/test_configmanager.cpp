#include "configmanager.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <QtTest>

class ConfigManagerTest : public QObject
{
    Q_OBJECT

private slots:
    void loadCustomConfigAndSanitize();
    void geoRoundTripKeepsPrecision();
};

void ConfigManagerTest::loadCustomConfigAndSanitize()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString filePath = tempDir.filePath(QStringLiteral("config.json"));
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));

    const QJsonObject root{
        {QStringLiteral("geo"), QJsonObject{{QStringLiteral("baseLatitudeDeg"), 31.223}, {QStringLiteral("baseLongitudeDeg"), 121.481}}},
        {QStringLiteral("control"),
         QJsonObject{{QStringLiteral("arrivalDistanceThreshold"), -3.0},
                     {QStringLiteral("headingSlowdownFactor"), 3.2},
                     {QStringLiteral("nearTargetDistanceMultiplier"), 0.1},
                     {QStringLiteral("nearTargetSpeedMultiplier"), 5.0},
                     {QStringLiteral("maxLinearSpeed"), -1.0},
                     {QStringLiteral("maxAngularSpeed"), -1.0}}},
        {QStringLiteral("video"),
         QJsonObject{{QStringLiteral("backend"), QStringLiteral("bad_backend")},
                     {QStringLiteral("controlBaseUrl"), QStringLiteral(" http://192.168.31.7:18080/ ")},
                     {QStringLiteral("streamOptions"),
                      QJsonArray{
                          QJsonObject{{QStringLiteral("name"), QStringLiteral(" 前置相机 ")},
                                      {QStringLiteral("url"), QStringLiteral(" http://192.168.31.13:18080/camera/stream.mjpeg ")}},
                          QJsonObject{{QStringLiteral("name"), QStringLiteral("坏地址")},
                                      {QStringLiteral("url"), QStringLiteral("://bad-url")}}
                      }},
                     {QStringLiteral("previewWidth"), 99},
                     {QStringLiteral("previewHeight"), 99999},
                     {QStringLiteral("previewFps"), 999},
                     {QStringLiteral("recordMode"), QStringLiteral("unsupported_mode")},
                     {QStringLiteral("recordCodec"), QStringLiteral("bad_codec")},
                     {QStringLiteral("reconnectIntervalMs"), 10}}},
        {QStringLiteral("network"), QJsonObject{{QStringLiteral("statusPollIntervalMs"), 1}}},
        {QStringLiteral("rowWork"),
         QJsonObject{{QStringLiteral("enabled"), true},
                     {QStringLiteral("gatewayBaseUrl"), QStringLiteral(" http://192.168.31.13:18120/ ")},
                     {QStringLiteral("statusPollIntervalMs"), 1},
                     {QStringLiteral("commandTimeoutMs"), 10},
                     {QStringLiteral("autoRefreshPlanStatus"), false}}}
    };

    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    file.close();

    ConfigManager &cfg = ConfigManager::instance();
    cfg.setConfigFilePath(filePath);

    QVERIFY(cfg.loadedFromFile());
    QCOMPARE(cfg.configFilePath(), filePath);
    QVERIFY(cfg.control().arrivalDistanceThreshold >= 0.01);
    QVERIFY(cfg.control().headingSlowdownFactor <= 1.0);
    QVERIFY(cfg.control().nearTargetDistanceMultiplier >= 1.0);
    QVERIFY(cfg.control().nearTargetSpeedMultiplier <= 1.0);
    QVERIFY(cfg.control().maxLinearSpeed >= 0.0);
    QVERIFY(cfg.control().maxAngularSpeed >= 0.0);
    QVERIFY(cfg.video().reconnectIntervalMs >= 200);
    QCOMPARE(cfg.video().backend, QStringLiteral("mjpeg_http"));
    QCOMPARE(cfg.video().controlBaseUrl, QStringLiteral("http://192.168.31.7:18080/"));
    QCOMPARE(cfg.video().streamOptions.size(), 1);
    QCOMPARE(cfg.video().streamOptions.first().name, QStringLiteral("前置相机"));
    QCOMPARE(cfg.video().streamOptions.first().url, QStringLiteral("http://192.168.31.13:18080/camera/stream.mjpeg"));
    QVERIFY(cfg.video().previewWidth >= 320);
    QVERIFY(cfg.video().previewHeight <= 3040);
    QVERIFY(cfg.video().previewFps <= 120);
    QCOMPARE(cfg.video().recordMode, QStringLiteral("host_opencv"));
    QCOMPARE(cfg.video().recordCodec, QStringLiteral("MJPG"));
    QVERIFY(cfg.network().statusPollIntervalMs >= 50);
    QVERIFY(cfg.rowWork().enabled);
    QCOMPARE(cfg.rowWork().gatewayBaseUrl, QStringLiteral("http://192.168.31.13:18120/"));
    QVERIFY(cfg.rowWork().statusPollIntervalMs >= 100);
    QVERIFY(cfg.rowWork().commandTimeoutMs >= 1000);
    QVERIFY(!cfg.rowWork().autoRefreshPlanStatus);

    cfg.setConfigFilePath(QString());
}

void ConfigManagerTest::geoRoundTripKeepsPrecision()
{
    ConfigManager &cfg = ConfigManager::instance();
    const QPointF local(12.345, -67.890);
    double lat = 0.0;
    double lon = 0.0;
    cfg.localToGeo(local, lat, lon);
    const QPointF reconstructed = cfg.geoToLocal(lat, lon);

    QVERIFY(qAbs(reconstructed.x() - local.x()) < 1e-6);
    QVERIFY(qAbs(reconstructed.y() - local.y()) < 1e-6);
}

QTEST_MAIN(ConfigManagerTest)
#include "test_configmanager.moc"
