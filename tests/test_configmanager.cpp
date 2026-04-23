#include "configmanager.h"

#include <QFile>
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
                     {QStringLiteral("previewWidth"), 99},
                     {QStringLiteral("previewHeight"), 99999},
                     {QStringLiteral("previewFps"), 999},
                     {QStringLiteral("recordMode"), QStringLiteral("unsupported_mode")},
                     {QStringLiteral("recordCodec"), QStringLiteral("bad_codec")},
                     {QStringLiteral("reconnectIntervalMs"), 10}}},
        {QStringLiteral("network"), QJsonObject{{QStringLiteral("statusPollIntervalMs"), 1}}}
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
    QVERIFY(cfg.video().previewWidth >= 320);
    QVERIFY(cfg.video().previewHeight <= 3040);
    QVERIFY(cfg.video().previewFps <= 120);
    QCOMPARE(cfg.video().recordMode, QStringLiteral("host_opencv"));
    QCOMPARE(cfg.video().recordCodec, QStringLiteral("MJPG"));
    QVERIFY(cfg.network().statusPollIntervalMs >= 50);

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
