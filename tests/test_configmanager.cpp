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
    void envOverridesSecretsAndReportsSchemaWarnings();
    void geoRoundTripKeepsPrecision();
    void unsupportedSchemaDoesNotMigrateOrEnableServices();
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
        {QStringLiteral("routePlanning"),
         QJsonObject{{QStringLiteral("minEdgeCost"), -1.0},
                     {QStringLiteral("edgePenalty"), -2.0},
                     {QStringLiteral("arcPenalty"), 0.25}}},
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
                     {QStringLiteral("autoRefreshPlanStatus"), false}}},
        {QStringLiteral("database"),
         QJsonObject{{QStringLiteral("backend"), QStringLiteral("bad_backend")},
                     {QStringLiteral("connectionName"), QStringLiteral(" ")},
                     {QStringLiteral("busyTimeoutMs"), 1},
                     {QStringLiteral("cacheSizePages"), 1},
                     {QStringLiteral("connectTimeoutMs"), 1},
                     {QStringLiteral("reconnectIntervalMs"), 1},
                     {QStringLiteral("enableTelemetryTables"), true}}}
    };

    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    file.close();

    ConfigManager &cfg = ConfigManager::instance();
    cfg.setConfigFilePath(filePath);

    QVERIFY(cfg.loadedFromFile());
    QVERIFY(QFile::exists(filePath + ".schema1.bak"));
    QFile migrated(filePath); QVERIFY(migrated.open(QIODevice::ReadOnly));
    const auto current = QJsonDocument::fromJson(migrated.readAll()).object();
    QCOMPARE(current["schemaVersion"].toInt(), 2);
    QVERIFY(!current.contains("control")); QVERIFY(!current.contains("rowWork"));
    QVERIFY(current.contains("taskDefaults")); migrated.close();
    QCOMPARE(cfg.configFilePath(), filePath);
    QVERIFY(cfg.manualControl().maxLinearSpeed >= 0.0);
    QVERIFY(cfg.manualControl().maxAngularSpeed >= 0.0);
    QVERIFY(cfg.routePlanning().minEdgeCost > 0.0);
    QVERIFY(cfg.routePlanning().edgePenalty >= 0.0);
    QCOMPARE(cfg.routePlanning().arcPenalty, 0.25);
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
    QCOMPARE(cfg.database().backend, QStringLiteral("sqlite"));
    QCOMPARE(cfg.database().connectionName, QStringLiteral("tenco_main"));
    QVERIFY(cfg.database().useWAL);
    QVERIFY(cfg.database().foreignKeys);
    QVERIFY(cfg.database().busyTimeoutMs >= 100);
    QVERIFY(cfg.database().cacheSizePages >= 64);
    QVERIFY(cfg.database().connectTimeoutMs >= 500);
    QVERIFY(cfg.database().reconnectIntervalMs >= 500);
    QVERIFY(cfg.database().enableTelemetryTables);

    cfg.setConfigFilePath(QString());
}

void ConfigManagerTest::envOverridesSecretsAndReportsSchemaWarnings()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString filePath = tempDir.filePath(QStringLiteral("config.json"));
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const QJsonObject root{
        {QStringLiteral("unknownTopLevel"), true},
        {QStringLiteral("network"),
         QJsonObject{{QStringLiteral("authToken"), QStringLiteral("file-token")},
                     {QStringLiteral("websocketUrl"), 42}}},
        {QStringLiteral("database"),
         QJsonObject{{QStringLiteral("password"), QStringLiteral("file-password")}}},
        {QStringLiteral("routePlanning"),
         QJsonObject{{QStringLiteral("minEdgeCost"), 0.5},
                     {QStringLiteral("edgePenalty"), 0.2},
                     {QStringLiteral("arcPenalty"), 0.7}}},
    };
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    file.close();

    qputenv("TENCO_AUTH_TOKEN", QByteArrayLiteral("env-token"));
    qputenv("TENCO_DATABASE_PASSWORD", QByteArrayLiteral("env-db-password"));

    ConfigManager &cfg = ConfigManager::instance();
    cfg.setConfigFilePath(filePath);

    QCOMPARE(cfg.network().authToken, QStringLiteral("env-token"));
    QCOMPARE(cfg.database().password, QStringLiteral("env-db-password"));
    QCOMPARE(cfg.routePlanning().minEdgeCost, 0.5);
    QCOMPARE(cfg.routePlanning().edgePenalty, 0.2);
    QCOMPARE(cfg.routePlanning().arcPenalty, 0.7);
    QVERIFY(!cfg.validationWarnings().isEmpty());

    qunsetenv("TENCO_AUTH_TOKEN");
    qunsetenv("TENCO_DATABASE_PASSWORD");
    cfg.setConfigFilePath(QString());
}

void ConfigManagerTest::unsupportedSchemaDoesNotMigrateOrEnableServices()
{
    QTemporaryDir dir;
    auto &cfg = ConfigManager::instance();
    for (const auto &schema : QJsonArray{0, 3, 1.5, "2", true, QJsonValue(QJsonValue::Null)}) {
        const auto path = dir.filePath("invalid.json");
        const auto bytes = QJsonDocument(QJsonObject{{"schemaVersion", schema},
            {"tracking", QJsonObject{{"enabled", true}}}}).toJson();
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(bytes); file.close();
        cfg.setConfigFilePath(path); cfg.reload();
        QVERIFY(!cfg.loadedFromFile());
        QVERIFY(!cfg.tracking().enabled);
        QVERIFY(!cfg.poseSource().enabled);
        ConfigManager::ConfigSnapshot snapshot;
        QVERIFY(!cfg.loadSnapshotFromFile(path, &snapshot));
        QVERIFY(!QFile::exists(path + ".schema1.bak"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), bytes);
    }
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
