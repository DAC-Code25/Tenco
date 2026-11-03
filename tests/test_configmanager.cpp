#include "configmanager.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDir>

#include <QtTest>

class ConfigManagerTest : public QObject
{
    Q_OBJECT

private slots:
    void drivingSpeedBoundsAndLegacyMigration();
    void loadCustomConfigAndSanitize();
    void retiredVideoChoicesRemainOpaqueAndNeverOverrideThePreview();
    void envOverridesSecretsAndReportsSchemaWarnings();
    void legacyNavigationFieldsRetireOnlyOnExplicitSave();
    void importedLegacyFieldsAreBackedUpAndCannotRevive();
    void restoredLegacyFieldsAreBackedUpAndCannotRevive();
    void schemaOnePartialManualSettingsMatchLoadImportAndRestore();
    void unsupportedSchemaDoesNotMigrateOrEnableServices();
    void savesPreserveCredentialsUnknownFieldsAndBackupExactOriginal();
    void backupFailureAndExternalEditsCannotOverwriteConfiguration();
    void restoreVerifiesDigestAndBacksUpCurrentBytes();
    void importedUnknownFieldsSurviveAndDefaultPathIgnoresWorkingDirectory();
};

static void writeConfigBytes(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(file.write(bytes), bytes.size());
}

static QByteArray readConfigBytes(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

void ConfigManagerTest::drivingSpeedBoundsAndLegacyMigration()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto &cfg = ConfigManager::instance();
    const auto path = dir.filePath("config.json");
    const QList<QJsonObject> inputs{
        {{"schemaVersion", 2}, {"manualControl", QJsonObject{{"maxLinearSpeed", 5.0}}},
          {"taskDefaults", QJsonObject{{"speedLimit", .05}}}},
        {{"schemaVersion", 2}, {"manualControl", QJsonObject{{"maxLinearSpeed", 0.0}}},
          {"taskDefaults", QJsonObject{{"speedLimit", .20}}}},
        {{"schemaVersion", 2}, {"taskDefaults", QJsonObject{{"speedLimit", 2.0}}}},
        {{"schemaVersion", 1}, {"control", QJsonObject{{"maxLinearSpeed", .60}}}},
        {{"schemaVersion", 2}},
        {{"schemaVersion", 2}, {"manualControl", QJsonObject{{"maxLinearSpeed", .05}}}}
    };
    const QList<double> expectedManual{1.50, 0.0, 1.50, .60, 1.50, .10};
    for (qsizetype i = 0; i < inputs.size(); ++i) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(QJsonDocument(inputs[i]).toJson()); file.close();
        cfg.setConfigFilePath(path); cfg.reload(); QVERIFY(cfg.loadedFromFile());
        QCOMPARE(cfg.manualControl().maxLinearSpeed, expectedManual[i]);
        QCOMPARE(QJsonDocument::fromJson(readConfigBytes(path)).object(), inputs[i]);
        QVERIFY(cfg.backupFilePaths().isEmpty());
    }
    cfg.setConfigFilePath(QString());
}

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
    QVERIFY(!QFile::exists(filePath + ".schema1.bak"));
    QVERIFY(cfg.backupFilePaths().isEmpty());
    QCOMPARE(QJsonDocument::fromJson(readConfigBytes(filePath)).object(), root);
    QVERIFY(cfg.saveSnapshot(cfg.snapshot()));
    QVERIFY(!cfg.backupFilePaths().isEmpty());
    QCOMPARE(QJsonDocument::fromJson(readConfigBytes(cfg.latestBackupFilePath())).object(), root);
    QFile migrated(filePath); QVERIFY(migrated.open(QIODevice::ReadOnly));
    const auto current = QJsonDocument::fromJson(migrated.readAll()).object();
    QCOMPARE(current["schemaVersion"].toInt(), 2);
    QVERIFY(!current.contains("control")); QVERIFY(!current.contains("rowWork"));
    QVERIFY(!current.contains("taskDefaults")); QVERIFY(!current.contains("geo")); migrated.close();
    QCOMPARE(cfg.configFilePath(), filePath);
    QVERIFY(cfg.manualControl().maxLinearSpeed >= 0.0);
    QVERIFY(cfg.manualControl().maxAngularSpeed >= 0.0);
    QVERIFY(cfg.routePlanning().minEdgeCost > 0.0);
    QVERIFY(cfg.routePlanning().edgePenalty >= 0.0);
    QCOMPARE(cfg.routePlanning().arcPenalty, 0.25);
    QVERIFY(cfg.video().reconnectIntervalMs >= 200);
    QCOMPARE(cfg.video().backend, QStringLiteral("mjpeg_http"));
    QCOMPARE(cfg.video().controlBaseUrl, QStringLiteral("http://192.168.31.7:18080/"));
    QCOMPARE(current["video"].toObject()["streamOptions"], root["video"].toObject()["streamOptions"]);
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

void ConfigManagerTest::retiredVideoChoicesRemainOpaqueAndNeverOverrideThePreview()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto &cfg = ConfigManager::instance();
    const auto path = directory.filePath(QStringLiteral("config.json"));
    const QJsonArray legacyChoices{
        QJsonObject{{"name", "previous-camera"}, {"url", "http://127.0.0.1:2/old"},
                    {"futureField", QJsonObject{{"preserve", true}}}},
        QStringLiteral("old-unrecognized-entry")
    };
    const QJsonObject document{{"schemaVersion", 2},
        {"video", QJsonObject{{"streamUrl", "http://127.0.0.1:1/color"},
                               {"controlBaseUrl", "http://127.0.0.1:1"},
                               {"streamOptions", legacyChoices}}}};
    const auto bytes = QJsonDocument(document).toJson();
    writeConfigBytes(path, bytes);
    cfg.setConfigFilePath(path);
    QCOMPARE(cfg.video().streamUrl, QStringLiteral("http://127.0.0.1:1/color"));
    QCOMPARE(cfg.video().controlBaseUrl, QStringLiteral("http://127.0.0.1:1"));
    QCOMPARE(readConfigBytes(path), bytes);

    auto changed = cfg.snapshot();
    changed.video.streamUrl = QStringLiteral("http://127.0.0.1:1/new-color");
    QVERIFY(cfg.saveSnapshot(changed, nullptr, true));
    const auto saved = QJsonDocument::fromJson(readConfigBytes(path)).object();
    QCOMPARE(saved["video"].toObject()["streamOptions"].toArray(), legacyChoices);
    QCOMPARE(cfg.video().streamUrl, changed.video.streamUrl);
    QCOMPARE(readConfigBytes(cfg.latestBackupFilePath()), bytes);

    // Fresh configurations do not generate the retired field.
    const auto freshPath = directory.filePath(QStringLiteral("fresh.json"));
    writeConfigBytes(freshPath, QJsonDocument(QJsonObject{{"schemaVersion", 2}}).toJson());
    cfg.setConfigFilePath(freshPath);
    QVERIFY(cfg.saveSnapshot(cfg.snapshot(), nullptr, true));
    const auto fresh = QJsonDocument::fromJson(readConfigBytes(freshPath)).object();
    QVERIFY(!fresh["video"].toObject().contains("streamOptions"));
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
        QVERIFY(!cfg.saveSnapshot(cfg.snapshot()));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), bytes);
    }
    cfg.setConfigFilePath(QString());
}

void ConfigManagerTest::savesPreserveCredentialsUnknownFieldsAndBackupExactOriginal()
{
    QTemporaryDir dir;
    auto &cfg = ConfigManager::instance();
    const auto path = dir.filePath("config.json");
    const QByteArray original = "{\r\n \"schemaVersion\":2,\"network\":{\"authToken\":\"test-secret\",\"futureAuth\":{\"mode\":7}},\"futureModule\":{\"enabled\":true}}\r\n";
    writeConfigBytes(path, original);
    cfg.setConfigFilePath(path);
    QVERIFY(cfg.createStartupBackup());
    const auto startupBackup = cfg.latestBackupFilePath();
    QCOMPARE(readConfigBytes(startupBackup), original);
    QVERIFY(cfg.backupDirectoryPath().endsWith("backups/config/local"));
    auto snapshot = cfg.snapshot();
    snapshot.manualControl.maxAngularSpeed = .2;
    QString error;
    QSignalSpy applied(&cfg, &ConfigManager::configChanged);
    QVERIFY2(cfg.saveSnapshot(snapshot, &error, false), qPrintable(error));
    QCOMPARE(applied.size(), 0);
    QCOMPARE(cfg.manualControl().maxAngularSpeed, .5);
    QVERIFY(cfg.diskRevision() != cfg.runtimeRevision());
    QCOMPARE(cfg.diskRevision(), cfg.expectedDiskRevision());
    const auto savedBytes = readConfigBytes(path);
    const auto saved = QJsonDocument::fromJson(readConfigBytes(path)).object();
    QCOMPARE(saved["network"].toObject()["authToken"].toString(), "test-secret");
    QCOMPARE(saved["network"].toObject()["futureAuth"].toObject()["mode"].toInt(), 7);
    QCOMPARE(saved["futureModule"].toObject()["enabled"].toBool(), true);
    QCOMPARE(cfg.backupFilePaths().size(), 1); // Identical startup/save snapshots share verified bytes.
    for (const auto &backup : cfg.backupFilePaths()) {
        QCOMPARE(readConfigBytes(backup), original);
        const auto manifest = QJsonDocument::fromJson(readConfigBytes(backup + ".manifest.json")).object();
        QCOMPARE(manifest["sha256"].toString().toLatin1(), QCryptographicHash::hash(original, QCryptographicHash::Sha256).toHex());
    }
    QVERIFY2(cfg.saveSnapshot(snapshot, &error, true), qPrintable(error));
    QCOMPARE(applied.size(), 1);
    QCOMPARE(cfg.manualControl().maxAngularSpeed, .2);
    QCOMPARE(cfg.diskRevision(), cfg.runtimeRevision());
    QCOMPARE(readConfigBytes(path), savedBytes); // Applying saved values does not need another edit.
    cfg.setConfigFilePath({});
}

void ConfigManagerTest::backupFailureAndExternalEditsCannotOverwriteConfiguration()
{
    QTemporaryDir dir;
    auto &cfg = ConfigManager::instance();
    const auto path = dir.filePath("config.json");
    const QByteArray original = "{\"schemaVersion\":2,\"network\":{\"authToken\":\"keep-this-test-token\"}}\n";
    writeConfigBytes(path, original);
    cfg.setConfigFilePath(path);
    writeConfigBytes(dir.filePath("backups"), "blocks-backup-directory");
    auto snapshot = cfg.snapshot(); snapshot.manualControl.maxAngularSpeed = .2;
    QString error;
    QVERIFY(!cfg.saveSnapshot(snapshot, &error, true));
    QCOMPARE(readConfigBytes(path), original);
    QCOMPARE(cfg.manualControl().maxAngularSpeed, .5);
    QVERIFY(QFile::remove(dir.filePath("backups")));
    const QByteArray external = "{\"schemaVersion\":2,\"network\":{\"authToken\":\"external-test-token\"}}";
    writeConfigBytes(path, external);
    QVERIFY(!cfg.saveSnapshot(snapshot, &error, true));
    QCOMPARE(readConfigBytes(path), external);
    QVERIFY(cfg.backupFilePaths().isEmpty());
    // A file edited after save-only must not be overwritten by a later apply.
    writeConfigBytes(path, original);
    QVERIFY(cfg.saveSnapshot(snapshot, &error, false));
    QCOMPARE(cfg.diskRevision(), cfg.expectedDiskRevision());
    const auto pendingRevision = cfg.expectedDiskRevision();
    writeConfigBytes(path, external);
    QVERIFY(cfg.diskRevision() != pendingRevision);
    QVERIFY(!cfg.saveSnapshot(snapshot, &error, true));
    QCOMPARE(readConfigBytes(path), external);
    QCOMPARE(cfg.manualControl().maxAngularSpeed, .5);
    cfg.setConfigFilePath({});
}

void ConfigManagerTest::restoreVerifiesDigestAndBacksUpCurrentBytes()
{
    QTemporaryDir dir;
    auto &cfg = ConfigManager::instance();
    const auto path = dir.filePath("config.json");
    const QByteArray original = "{\"schemaVersion\":2,\"network\":{\"authToken\":\"original-test-token\"}}\r\n";
    writeConfigBytes(path, original); cfg.setConfigFilePath(path);
    QVERIFY(cfg.createBackup());
    const auto backup = cfg.latestBackupFilePath();
    const auto snapshot = cfg.snapshot();
    auto changed = snapshot; changed.network.authToken = "changed-test-token";
    QVERIFY(cfg.saveSnapshot(changed, nullptr, true));
    const auto changedBytes = readConfigBytes(path);
    writeConfigBytes(backup, "{\"schemaVersion\":2,\"tampered\":true}");
    QString error;
    QVERIFY(!cfg.restoreBackup(backup, &error, true));
    QCOMPARE(readConfigBytes(path), changedBytes);
    writeConfigBytes(backup, original);
    QVERIFY2(cfg.restoreBackup(backup, &error, true), qPrintable(error));
    QCOMPARE(readConfigBytes(path), original);
    QCOMPARE(cfg.network().authToken, "original-test-token");
    bool foundPreRestore = false;
    for (const auto &candidate : cfg.backupFilePaths()) {
        const auto manifest = QJsonDocument::fromJson(readConfigBytes(candidate + ".manifest.json")).object();
        if (manifest["reason"].toString() == "before_restore") {
            QCOMPARE(readConfigBytes(candidate), changedBytes); foundPreRestore = true;
        }
    }
    QVERIFY(foundPreRestore);
    cfg.setConfigFilePath({});
}

void ConfigManagerTest::importedUnknownFieldsSurviveAndDefaultPathIgnoresWorkingDirectory()
{
    QTemporaryDir dir;
    auto &cfg = ConfigManager::instance();
    const auto path = dir.filePath("config.json");
    writeConfigBytes(path, "{\"schemaVersion\":2}"); cfg.setConfigFilePath(path);
    const auto imported = dir.filePath("imported.json");
    writeConfigBytes(imported, "{\"schemaVersion\":2,\"network\":{\"authToken\":\"imported-test-token\",\"future\":true},\"futureTop\":[1,2]}");
    ConfigManager::ConfigSnapshot snapshot;
    QVERIFY(cfg.loadSnapshotFromFile(imported, &snapshot));
    QVERIFY(cfg.saveSnapshot(snapshot, nullptr, true));
    const auto saved = QJsonDocument::fromJson(readConfigBytes(path)).object();
    QCOMPARE(saved["network"].toObject()["authToken"].toString(), "imported-test-token");
    QVERIFY(saved["network"].toObject()["future"].toBool());
    QCOMPARE(saved["futureTop"].toArray(), QJsonArray({1,2}));
    const auto oldCwd = QDir::currentPath();
    QVERIFY(QDir::setCurrent(dir.path()));
    cfg.setConfigFilePath({});
    QCOMPARE(QFileInfo(cfg.configFilePath()).absoluteFilePath(), QDir(QCoreApplication::applicationDirPath()).filePath("config.json"));
    QVERIFY(QDir::setCurrent(oldCwd));
}

static QJsonObject retiredConfiguration(int schema = 2)
{
    return {{"schemaVersion", schema},
        {"geo", QJsonObject{{"baseLatitudeDeg", 30.25}, {"baseLongitudeDeg", 120.5}}},
        {"taskDefaults", QJsonObject{{"speedLimit", 1.2}, {"goalToleranceMeters", .4}}},
        {"control", QJsonObject{{"maxAngularSpeed", .37}, {"arrivalDistanceThreshold", .4}}},
        {"rowWork", QJsonObject{{"gatewayBaseUrl", "http://retired.invalid"}}},
        {"vehicle", QJsonObject{{"wheelTrack", .9}}},
        {"network", QJsonObject{{"authToken", "preserved-legacy-test-token"}}},
        {"futureModule", QJsonObject{{"keep", 19}}}};
}

static void verifyRetiredSettingsAbsent(const QJsonObject &document)
{
    for (const auto &key : {"geo", "taskDefaults", "control", "rowWork", "vehicle"})
        QVERIFY2(!document.contains(key), key);
}

static bool hasExactBackup(const ConfigManager &config, const QByteArray &bytes)
{
    for (const auto &path : config.backupFilePaths())
        if (readConfigBytes(path) == bytes) return true;
    return false;
}

void ConfigManagerTest::legacyNavigationFieldsRetireOnlyOnExplicitSave()
{
    QTemporaryDir dir;
    auto &cfg = ConfigManager::instance();
    const auto path = dir.filePath("config.json");
    const auto original = QJsonDocument(retiredConfiguration()).toJson();
    writeConfigBytes(path, original);
    cfg.setConfigFilePath(path);
    cfg.reload();
    QCOMPARE(readConfigBytes(path), original);
    QVERIFY(cfg.backupFilePaths().isEmpty());
    QVERIFY(cfg.validationWarnings().join(" ").contains(QStringLiteral("已停用")));
    QCOMPARE(cfg.manualControl().maxAngularSpeed, .5); // Schema 2 control is retired too.
    const auto token = cfg.network().authToken;
    QVERIFY(cfg.saveSnapshot(cfg.snapshot(), nullptr, true));
    const auto saved = QJsonDocument::fromJson(readConfigBytes(path)).object();
    verifyRetiredSettingsAbsent(saved);
    QCOMPARE(saved["network"].toObject()["authToken"].toString(), token);
    QCOMPARE(saved["futureModule"].toObject()["keep"].toInt(), 19);
    QVERIFY(hasExactBackup(cfg, original));
    // Even a preserved/merged document cannot recreate a second authority.
    auto stale = cfg.snapshot();
    stale.sourceDocument = retiredConfiguration();
    QVERIFY(cfg.saveSnapshot(stale, nullptr, true));
    verifyRetiredSettingsAbsent(QJsonDocument::fromJson(readConfigBytes(path)).object());
    cfg.setConfigFilePath({});
}

void ConfigManagerTest::importedLegacyFieldsAreBackedUpAndCannotRevive()
{
    QTemporaryDir dir;
    auto &cfg = ConfigManager::instance();
    const auto path = dir.filePath("config.json");
    const QByteArray current = "{\"schemaVersion\":2,\"network\":{\"authToken\":\"current-test-token\"}}\r\n";
    writeConfigBytes(path, current);
    cfg.setConfigFilePath(path);
    const auto inputPath = dir.filePath("import.json");
    const auto importedBytes = QJsonDocument(retiredConfiguration(1)).toJson();
    writeConfigBytes(inputPath, importedBytes);
    ConfigManager::ConfigSnapshot imported;
    QVERIFY(cfg.loadSnapshotFromFile(inputPath, &imported));
    QCOMPARE(imported.originalImportBytes, importedBytes);
    QCOMPARE(readConfigBytes(path), current);
    QVERIFY(cfg.backupFilePaths().isEmpty());
    QCOMPARE(cfg.network().authToken, QStringLiteral("current-test-token"));
    QCOMPARE(imported.manualControl.maxAngularSpeed, .37);
    QVERIFY(cfg.saveSnapshot(imported, nullptr, true));
    const auto saved = QJsonDocument::fromJson(readConfigBytes(path)).object();
    verifyRetiredSettingsAbsent(saved);
    QCOMPARE(saved["schemaVersion"].toInt(), 2);
    QCOMPARE(cfg.manualControl().maxAngularSpeed, .37);
    QCOMPARE(cfg.network().authToken, QStringLiteral("preserved-legacy-test-token"));
    QCOMPARE(saved["futureModule"].toObject()["keep"].toInt(), 19);
    QVERIFY(hasExactBackup(cfg, current));
    QVERIFY(hasExactBackup(cfg, importedBytes));
    QCOMPARE(readConfigBytes(inputPath), importedBytes);
    cfg.reload();
    verifyRetiredSettingsAbsent(cfg.snapshot().sourceDocument);
    cfg.setConfigFilePath({});
}

void ConfigManagerTest::restoredLegacyFieldsAreBackedUpAndCannotRevive()
{
    QTemporaryDir dir;
    auto &cfg = ConfigManager::instance();
    const auto path = dir.filePath("config.json");
    const auto legacyBytes = QJsonDocument(retiredConfiguration(1)).toJson();
    writeConfigBytes(path, legacyBytes);
    cfg.setConfigFilePath(path);
    QVERIFY(cfg.createBackup());
    const auto legacyBackup = cfg.latestBackupFilePath();
    auto changed = cfg.snapshot();
    changed.network.authToken = "newer-test-token";
    changed.manualControl.maxAngularSpeed = .6;
    QVERIFY(cfg.saveSnapshot(changed, nullptr, true));
    const auto beforeRestore = readConfigBytes(path);
    QVERIFY(cfg.restoreBackup(legacyBackup, nullptr, true));
    const auto restored = QJsonDocument::fromJson(readConfigBytes(path)).object();
    verifyRetiredSettingsAbsent(restored);
    QCOMPARE(restored["schemaVersion"].toInt(), 2);
    QCOMPARE(cfg.network().authToken, QStringLiteral("preserved-legacy-test-token"));
    QCOMPARE(cfg.manualControl().maxAngularSpeed, .37);
    QCOMPARE(restored["futureModule"].toObject()["keep"].toInt(), 19);
    QVERIFY(hasExactBackup(cfg, legacyBytes));
    QVERIFY(hasExactBackup(cfg, beforeRestore));
    QCOMPARE(readConfigBytes(legacyBackup), legacyBytes);
    cfg.reload();
    verifyRetiredSettingsAbsent(cfg.snapshot().sourceDocument);
    cfg.setConfigFilePath({});
}

void ConfigManagerTest::schemaOnePartialManualSettingsMatchLoadImportAndRestore()
{
    QTemporaryDir dir;
    auto &cfg = ConfigManager::instance();
    const auto path = dir.filePath("config.json");
    const QJsonObject legacy{{"schemaVersion", 1},
        {"manualControl", QJsonObject{{"maxLinearSpeed", .3}, {"maxAngularSpeed", .6},
                                      {"manualMotionRepeatIntervalMs", 80}}},
        {"control", QJsonObject{{"maxLinearSpeed", .2}}}};
    const auto bytes = QJsonDocument(legacy).toJson();
    writeConfigBytes(path, bytes);
    cfg.setConfigFilePath(path);
    QCOMPARE(readConfigBytes(path), bytes);
    QCOMPARE(cfg.manualControl().maxLinearSpeed, .2); // Existing schema 1 keys retain precedence.
    QCOMPARE(cfg.manualControl().maxAngularSpeed, .6);
    QCOMPARE(cfg.manualControl().manualMotionRepeatIntervalMs, 80);
    ConfigManager::ConfigSnapshot imported;
    QVERIFY(cfg.loadSnapshotFromFile(path, &imported));
    QCOMPARE(imported.manualControl.manualMotionRepeatIntervalMs, 80);
    QVERIFY(cfg.createBackup());
    const auto backup = cfg.latestBackupFilePath();
    QVERIFY(cfg.saveSnapshot(imported, nullptr, true));
    QCOMPARE(cfg.manualControl().manualMotionRepeatIntervalMs, 80);
    QVERIFY(cfg.restoreBackup(backup, nullptr, true));
    QCOMPARE(cfg.manualControl().maxLinearSpeed, .2);
    QCOMPARE(cfg.manualControl().maxAngularSpeed, .6);
    QCOMPARE(cfg.manualControl().manualMotionRepeatIntervalMs, 80);
    verifyRetiredSettingsAbsent(cfg.snapshot().sourceDocument);
    QVERIFY(hasExactBackup(cfg, bytes));
    cfg.setConfigFilePath({});
}

QTEST_MAIN(ConfigManagerTest)
#include "test_configmanager.moc"
