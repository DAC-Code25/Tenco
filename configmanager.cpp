#include "configmanager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QDateTime>
#include <QCryptographicHash>
#include <QUuid>
#include <QStringList>
#include <QUrl>
#include <QtGlobal>
#include <utility>
#include <algorithm>
#include <cmath>

Q_LOGGING_CATEGORY(lcConfigManager, "tenco.config")

namespace {
constexpr const char *kDefaultWebSocketUrl = "ws://192.168.31.7:1202";
constexpr const char *kDefaultStatusReadUrl = "http://192.168.31.7:9999/table/reads";
constexpr const char *kDefaultVideoBackend = "mjpeg_http";
constexpr const char *kDefaultVideoRecordMode = "host_opencv";
constexpr const char *kDefaultVideoRecordCodec = "MJPG";
constexpr int kDefaultStatusPollIntervalMs = 100;
constexpr int kDefaultStatusRequestTimeoutMs = 3000;
constexpr int kDefaultStatusMaxBackoffMs = 5000;
constexpr int kDefaultChassisReconnectIntervalMs = 1000;
constexpr int kDefaultChassisReconnectMaxIntervalMs = 15000;
constexpr int kMinStatusPollIntervalMs = 50;
constexpr int kMaxStatusPollIntervalMs = 5000;
constexpr const char *kDefaultGimbalPlcHost = "192.168.31.120";
constexpr int kDefaultGimbalPlcPort = 502;
constexpr int kDefaultGimbalUnitId = 255;
constexpr int kDefaultGimbalRequestTimeoutMs = 1000;
constexpr int kDefaultGimbalStatusPollIntervalMs = 300;
constexpr int kDefaultCameraRequestTimeoutMs = 5000;
constexpr int kDefaultManualMotionRepeatIntervalMs = 40;
constexpr double kDefaultRouteMinEdgeCost = 1e-3;
constexpr double kDefaultRouteEdgePenalty = 0.01;
constexpr double kDefaultRouteArcPenalty = 0.05;
constexpr int kDefaultGimbalSafetyStopTimeoutMs = 1500;
constexpr qint64 kDefaultLogMaxFileBytes = 5 * 1024 * 1024;
constexpr qint64 kMinLogMaxFileBytes = 256 * 1024;
constexpr qint64 kMaxLogMaxFileBytes = 256 * 1024 * 1024;
constexpr int kDefaultLogMaxBackupFiles = 10;
constexpr const char *kDefaultDatabaseBackend = "sqlite";
constexpr const char *kDefaultDatabaseConnectionName = "tenco_main";
constexpr const char *kDefaultDatabaseName = "tenco";
constexpr int kDefaultDatabasePort = 5432;
constexpr int kDefaultDatabaseBusyTimeoutMs = 5000;
constexpr int kDefaultDatabaseCacheSizePages = 2000;
constexpr int kDefaultDatabaseConnectTimeoutMs = 5000;
constexpr int kDefaultDatabaseReconnectIntervalMs = 3000;

int configurationSchema(const QJsonObject &root)
{
    const auto value = root.value("schemaVersion");
    if (value.isUndefined()) return 1;
    if (value == QJsonValue(1)) return 1;
    if (value == QJsonValue(2)) return 2;
    return 0;
}

QString defaultConfigPath()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("config.json"));
}

QString bytesRevision(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QJsonObject mergeDocuments(QJsonObject base, const QJsonObject &overlay)
{
    for (auto it = overlay.begin(); it != overlay.end(); ++it)
        base.insert(it.key(), it->isObject() && base.value(it.key()).isObject() ?
            QJsonValue(mergeDocuments(base.value(it.key()).toObject(), it->toObject())) : it.value());
    return base;
}

QStringList retiredLocalSections(const QJsonObject &root)
{
    QStringList sections;
    for (const auto &key : {QStringLiteral("geo"), QStringLiteral("taskDefaults"),
                            QStringLiteral("control"), QStringLiteral("rowWork"), QStringLiteral("vehicle")})
        if (root.contains(key)) sections.append(key);
    return sections;
}

QJsonObject retireLocalNavigationSettings(QJsonObject root)
{
    // Schema 1 stored the independent manual channel under control. Preserve
    // those three settings, but never migrate automatic settings to the IPC.
    if (configurationSchema(root) == 1) {
        auto manual = root.value("manualControl").toObject();
        const auto legacy = root.value("control").toObject();
        for (const auto &key : {QStringLiteral("maxLinearSpeed"), QStringLiteral("maxAngularSpeed"),
                                QStringLiteral("manualMotionRepeatIntervalMs")})
            if (legacy.contains(key)) manual.insert(key, legacy.value(key));
        if (!manual.isEmpty()) root.insert("manualControl", manual);
        root.insert("schemaVersion", 2);
    }
    for (const auto &key : retiredLocalSections(root)) root.remove(key);
    return root;
}
}

ConfigManager &ConfigManager::instance()
{
    static ConfigManager s_instance;
    s_instance.ensureLoaded();
    return s_instance;
}

void ConfigManager::reload()
{
    m_loaded = false;
    load();
    emit configChanged();
}

void ConfigManager::setConfigFilePath(const QString &path)
{
    if (m_configPathOverride != path.trimmed()) m_startupBackupCreated = false;
    m_configPathOverride = path.trimmed();
    reload();
}

ConfigManager::ConfigSnapshot ConfigManager::snapshot() const
{
    ConfigSnapshot snap;
    snap.sourceDocument = m_sourceDocument;
    snap.manualControl = m_manualControl;
    snap.poseSource = m_poseSource;
    snap.tracking = m_tracking;
    snap.routePlanning = m_routePlanning;
    snap.video = m_video;
    snap.network = m_network;
    snap.gimbal = m_gimbal;
    snap.logging = m_logging;
    snap.database = m_database;
    return snap;
}

bool ConfigManager::saveSnapshot(const ConfigSnapshot &snapshot, QString *errorMessage, bool applyAfterSave)
{
    if (m_writeBlocked || (!snapshot.sourceDocument.isEmpty() && configurationSchema(snapshot.sourceDocument) == 0)) {
        if (errorMessage) *errorMessage = tr("当前配置损坏或版本不受支持，禁止用默认值覆盖；请先选择有效备份恢复");
        return false;
    }
    if (!snapshot.originalImportBytes.isEmpty() && !retiredLocalSections(snapshot.sourceDocument).isEmpty()) {
        const auto imported = QJsonDocument::fromJson(snapshot.originalImportBytes);
        if (!imported.isObject() || imported.object() != snapshot.sourceDocument) {
            if (errorMessage) *errorMessage = tr("导入配置与预览来源不一致，未保存；请重新导入");
            return false;
        }
        if (!backupBytes(snapshot.originalImportBytes, QStringLiteral("before_import_migration"), errorMessage)) return false;
    }
    QFile current(m_configPath);
    QByteArray original;
    if (current.exists()) {
        if (!current.open(QIODevice::ReadOnly)) { if (errorMessage) *errorMessage = tr("无法读取现有配置，未覆盖"); return false; }
        original = current.readAll();
        current.close();
        const auto doc = QJsonDocument::fromJson(original);
        if (!doc.isObject() || configurationSchema(doc.object()) == 0 || original != m_diskBytes) {
            if (errorMessage) *errorMessage = tr("磁盘配置已变化、损坏或版本不支持，请先重新读取；未覆盖文件");
            return false;
        }
        if (!backupBytes(original, QStringLiteral("before_save"), errorMessage)) return false;
    }
    const ConfigSnapshot oldSnapshot = this->snapshot();
    const QString oldPath = m_configPath;
    const bool oldLoaded = m_loaded;
    const bool oldLoadedFromFile = m_loadedFromFile;
    m_sourceDocument = mergeDocuments(m_sourceDocument, snapshot.sourceDocument);

    m_manualControl = snapshot.manualControl;
    m_poseSource = snapshot.poseSource;
    m_tracking = snapshot.tracking;
    m_routePlanning = snapshot.routePlanning;
    m_video = snapshot.video;
    m_network = snapshot.network;
    m_gimbal = snapshot.gimbal;
    m_logging = snapshot.logging;
    m_database = snapshot.database;
    sanitizeConfig();

    const QString targetPath = m_configPath.isEmpty()
                                   ? (m_configPathOverride.isEmpty() ? defaultConfigPath() : m_configPathOverride)
                                   : m_configPath;
    QFile recheck(targetPath);
    const bool unchanged = original.isEmpty() ? !recheck.exists() :
        (recheck.open(QIODevice::ReadOnly) && recheck.readAll() == original);
    recheck.close();
    if (!unchanged && errorMessage) *errorMessage = tr("备份期间磁盘配置已变化，未覆盖文件");
    if (!unchanged || !saveToFile(targetPath, errorMessage)) {
        m_sourceDocument = oldSnapshot.sourceDocument;
        m_manualControl = oldSnapshot.manualControl;
        m_poseSource = oldSnapshot.poseSource;
        m_tracking = oldSnapshot.tracking;
        m_routePlanning = oldSnapshot.routePlanning;
        m_video = oldSnapshot.video;
        m_network = oldSnapshot.network;
        m_gimbal = oldSnapshot.gimbal;
        m_logging = oldSnapshot.logging;
        m_database = oldSnapshot.database;
        m_configPath = oldPath;
        m_loaded = oldLoaded;
        m_loadedFromFile = oldLoadedFromFile;
        return false;
    }

    QFile saved(targetPath);
    if (saved.open(QIODevice::ReadOnly)) m_diskBytes = saved.readAll();
    m_sourceDocument = QJsonDocument::fromJson(m_diskBytes).object();

    if (applyAfterSave) {
        m_configPath = targetPath;
        m_loaded = true;
        m_loadedFromFile = true;
        m_runtimeRevision = bytesRevision(m_diskBytes);
        emit configChanged();
    } else {
        m_manualControl = oldSnapshot.manualControl;
        m_poseSource = oldSnapshot.poseSource;
        m_tracking = oldSnapshot.tracking;
        m_routePlanning = oldSnapshot.routePlanning;
        m_video = oldSnapshot.video;
        m_network = oldSnapshot.network;
        m_gimbal = oldSnapshot.gimbal;
        m_logging = oldSnapshot.logging;
        m_database = oldSnapshot.database;
        m_configPath = oldPath;
        m_loaded = oldLoaded;
        m_loadedFromFile = oldLoadedFromFile;
    }

    return true;
}

bool ConfigManager::createStartupBackup(QString *errorMessage)
{
    ensureLoaded();
    if (m_startupBackupCreated) {
        return true;
    }
    if (!QFile::exists(m_configPath)) {
        m_startupBackupCreated = true;
        return true;
    }

    if (!createBackup(errorMessage, QStringLiteral("startup"))) return false;

    m_startupBackupCreated = true;
    return true;
}

bool ConfigManager::backupBytes(const QByteArray &bytes, const QString &reason, QString *errorMessage) const
{
    const QDir dir(backupDirectoryPath());
    if (!QDir().mkpath(dir.absolutePath())) { if (errorMessage) *errorMessage = tr("无法创建本机配置备份目录，未覆盖配置"); return false; }
    const auto revision = bytesRevision(bytes);
    const auto document = QJsonDocument::fromJson(bytes);
    const auto schema = document.isObject() ? configurationSchema(document.object()) : 0;
    // Reuse only a verified, byte-identical snapshot in the new directory.
    // Repeated starts must not create an unbounded pile of identical secrets.
    for (const auto &entry : dir.entryInfoList({"config_*.json"}, QDir::Files)) {
        if (entry.fileName().endsWith(".manifest.json")) continue;
        QFile metadata(entry.absoluteFilePath() + ".manifest.json");
        if (!metadata.open(QIODevice::ReadOnly)) continue;
        const auto manifest = QJsonDocument::fromJson(metadata.readAll()).object();
        if (manifest.value("schemaVersion") != QJsonValue(1) || manifest.value("sha256").toString() != revision ||
            manifest.value("file").toString() != entry.fileName() ||
            manifest.value("configurationSchemaVersion").toInt(-1) != schema) continue;
        QFile candidate(entry.absoluteFilePath());
        if (candidate.open(QIODevice::ReadOnly) && candidate.readAll() == bytes) return true;
    }
    const QString name = QStringLiteral("config_%1_%2.json")
        .arg(QDateTime::currentDateTimeUtc().toString("yyyyMMdd_HHmmss_zzz"), QUuid::createUuid().toString(QUuid::Id128));
    const QString target = dir.filePath(name);
    QSaveFile file(target);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        if (errorMessage) *errorMessage = tr("本机配置原字节备份失败，未覆盖配置");
        return false;
    }
    const QJsonObject manifest{{"schemaVersion", 1}, {"configurationSchemaVersion", schema},
        {"sha256", revision}, {"file", name}, {"reason", reason},
        {"createdAtUtc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
    QSaveFile metadata(target + ".manifest.json");
    const auto content = QJsonDocument(manifest).toJson();
    if (!metadata.open(QIODevice::WriteOnly) || metadata.write(content) != content.size() || !metadata.commit()) {
        QFile::remove(target);
        if (errorMessage) *errorMessage = tr("配置备份摘要写入失败，未覆盖配置");
        return false;
    }
    return true;
}

bool ConfigManager::createBackup(QString *errorMessage, const QString &reason)
{
    QFile file(m_configPath);
    if (!file.open(QIODevice::ReadOnly)) { if (errorMessage) *errorMessage = tr("无法读取本机配置以创建备份"); return false; }
    return backupBytes(file.readAll(), reason, errorMessage);
}

QString ConfigManager::diskRevision() const {
    QFile file(m_configPath);
    return file.open(QIODevice::ReadOnly) ? bytesRevision(file.readAll()) : QString();
}

QString ConfigManager::expectedDiskRevision() const
{
    return m_diskBytes.isEmpty() && !m_loadedFromFile && !m_writeBlocked ? QString() : bytesRevision(m_diskBytes);
}

QString ConfigManager::backupDirectoryPath() const
{
    const QFileInfo configInfo(m_configPath.isEmpty() ? defaultConfigPath() : m_configPath);
    return configInfo.absoluteDir().filePath(QStringLiteral("backups/config/local"));
}

QStringList ConfigManager::backupFilePaths() const
{
    QFileInfoList entries;
    const auto parent = QFileInfo(m_configPath).absoluteDir();
    for (const auto &directory : {backupDirectoryPath(), parent.filePath("config_backups")})
        for (const auto &entry : QDir(directory).entryInfoList({"config_*.json"}, QDir::Files))
            if (!entry.fileName().endsWith(".manifest.json")) entries.append(entry);
    std::sort(entries.begin(), entries.end(), [](const QFileInfo &a, const QFileInfo &b) {
        return a.lastModified() == b.lastModified() ? a.fileName() > b.fileName() : a.lastModified() > b.lastModified();
    });
    QStringList paths;
    for (const QFileInfo &info : entries) {
        paths.push_back(info.absoluteFilePath());
    }
    return paths;
}

QString ConfigManager::latestBackupFilePath() const
{
    const QStringList paths = backupFilePaths();
    return paths.isEmpty() ? QString() : paths.first();
}

bool ConfigManager::currentConfigMatchesLatestBackup() const
{
    const QString latestPath = latestBackupFilePath();
    if (latestPath.isEmpty() || m_configPath.isEmpty()) {
        return false;
    }
    QFile currentFile(m_configPath);
    QFile backupFile(latestPath);
    if (!currentFile.open(QIODevice::ReadOnly) || !backupFile.open(QIODevice::ReadOnly)) {
        return false;
    }
    return currentFile.readAll() == backupFile.readAll();
}

bool ConfigManager::loadSnapshotFromFile(const QString &path, ConfigSnapshot *snapshot, QString *errorMessage)
{
    if (!snapshot) {
        if (errorMessage) {
            *errorMessage = tr("配置快照输出为空");
        }
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorMessage) {
            *errorMessage = tr("无法读取配置文件：%1").arg(file.errorString());
        }
        return false;
    }

    const auto importedBytes = file.readAll();
    const QJsonDocument doc = QJsonDocument::fromJson(importedBytes);
    if (!doc.isObject()) {
        if (errorMessage) {
            *errorMessage = tr("配置文件不是合法 JSON 对象：%1").arg(path);
        }
        return false;
    }

    const int schema = configurationSchema(doc.object());
    if (schema < 1 || schema > 2) {
        if (errorMessage) *errorMessage = tr("不支持的配置版本");
        return false;
    }

    const ConfigSnapshot oldSnapshot = this->snapshot();

    loadDefaults();
    m_sourceDocument = doc.object();
    applyJsonObjectToCurrentConfig(doc.object());
    sanitizeConfig();
    *snapshot = this->snapshot();
    snapshot->originalImportBytes = importedBytes;

    m_manualControl = oldSnapshot.manualControl;
    m_poseSource = oldSnapshot.poseSource;
    m_tracking = oldSnapshot.tracking;
    m_routePlanning = oldSnapshot.routePlanning;
    m_video = oldSnapshot.video;
    m_network = oldSnapshot.network;
    m_gimbal = oldSnapshot.gimbal;
    m_logging = oldSnapshot.logging;
    m_database = oldSnapshot.database;
    m_sourceDocument = oldSnapshot.sourceDocument;
    return true;
}

bool ConfigManager::restoreLatestBackup(QString *errorMessage, bool applyAfterRestore)
{
    ensureLoaded();
    const QString latestPath = latestBackupFilePath();
    if (latestPath.isEmpty()) {
        if (errorMessage) {
            *errorMessage = tr("没有可恢复的配置备份");
        }
        return false;
    }

    return restoreBackup(latestPath, errorMessage, applyAfterRestore);
}

bool ConfigManager::restoreBackup(const QString &path, QString *errorMessage, bool applyAfterRestore)
{
    ensureLoaded();
    QFile source(path);
    if (!source.open(QIODevice::ReadOnly)) { if (errorMessage) *errorMessage = tr("无法读取所选备份"); return false; }
    const auto bytes = source.readAll();
    const auto document = QJsonDocument::fromJson(bytes);
    if (!document.isObject() || configurationSchema(document.object()) == 0) {
        if (errorMessage) *errorMessage = tr("备份配置损坏或版本不受支持，未恢复配置");
        return false;
    }
    const auto manifestPath = path + ".manifest.json";
    const bool legacy = QFileInfo(path).absoluteDir().absolutePath() ==
        QFileInfo(m_configPath).absoluteDir().filePath("config_backups");
    QFile manifestFile(manifestPath);
    if (!manifestFile.open(QIODevice::ReadOnly)) {
        if (!legacy) { if (errorMessage) *errorMessage = tr("备份缺少摘要清单，拒绝恢复；普通配置文件请通过导入预览"); return false; }
    } else {
        const auto manifest = QJsonDocument::fromJson(manifestFile.readAll()).object();
        if (manifest.value("schemaVersion") != QJsonValue(1) || manifest.value("file").toString() != QFileInfo(path).fileName() ||
            manifest.value("sha256").toString() != bytesRevision(bytes) ||
            manifest.value("configurationSchemaVersion").toInt() != configurationSchema(document.object())) {
            if (errorMessage) *errorMessage = tr("备份摘要或版本校验失败，未恢复配置");
            return false;
        }
    }
    const auto restoredDocument = retireLocalNavigationSettings(document.object());
    const auto restoredBytes = restoredDocument == document.object() ? bytes : QJsonDocument(restoredDocument).toJson();
    if (restoredBytes != bytes && !backupBytes(bytes, QStringLiteral("before_restore_migration"), errorMessage)) return false;
    if (QFile::exists(m_configPath) && !createBackup(errorMessage, QStringLiteral("before_restore"))) return false;
    if (!writeFileAtomically(restoredBytes, m_configPath, errorMessage)) return false;
    m_diskBytes = restoredBytes;
    m_sourceDocument = restoredDocument;
    m_writeBlocked = false;
    if (applyAfterRestore) reload();
    return true;
}

ConfigManager::ConfigManager(QObject *parent)
    : QObject(parent)
{
}

void ConfigManager::ensureLoaded()
{
    if (m_loaded) {
        return;
    }
    load();
}

void ConfigManager::load()
{
    const QString path = m_configPathOverride.isEmpty() ? defaultConfigPath() : m_configPathOverride;
    m_configPath = path;
    m_loadedFromFile = false;
    m_sourceDocument = {};
    m_diskBytes.clear();
    m_writeBlocked = false;
    loadDefaults();
    if (QFile::exists(path)) {
        loadFromFile(path);
    } else {
        m_validationWarnings.clear();
        qCWarning(lcConfigManager) << "Config file not found, fallback to defaults:" << path;
    }
    applyEnvironmentOverrides();
    sanitizeConfig();
    m_loaded = true;
    m_runtimeRevision = bytesRevision(m_diskBytes);
    qCInfo(lcConfigManager) << "Config loaded from" << m_configPath << ", from file?" << m_loadedFromFile;
}

void ConfigManager::loadFromFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        m_writeBlocked = true;
        loadDefaults();
        return;
    }

    const QByteArray raw = file.readAll();
    m_diskBytes = raw;
    file.close();

    const QJsonDocument doc = QJsonDocument::fromJson(raw);
    if (!doc.isObject()) {
        m_validationWarnings = {tr("配置文件不是合法 JSON 对象：%1").arg(path)};
        qCWarning(lcConfigManager) << "Invalid JSON object in config:" << path;
        m_poseSource.enabled = m_tracking.enabled = false;
        m_writeBlocked = true;
        return;
    }

    const QJsonObject root = doc.object();
    m_validationWarnings = validateJsonObject(root);
    for (const QString &warning : std::as_const(m_validationWarnings)) {
        qCWarning(lcConfigManager) << warning;
    }
    const int schema = configurationSchema(root);
    if (schema > 2 || schema < 1) { m_validationWarnings << tr("不支持的配置版本，禁止保存覆盖"); m_loadedFromFile = false; m_poseSource.enabled = m_tracking.enabled = false; m_writeBlocked = true; return; }
    m_sourceDocument = root;
    applyJsonObjectToCurrentConfig(root);
    m_loadedFromFile = true;
    sanitizeConfig();
    // Loading never rewrites operator-owned configuration. An explicit save or
    // restore retires duplicate navigation/origin fields after an exact backup.
    if (schema < 2)
        m_validationWarnings << tr("旧版本机配置已兼容读取；仅手动控制设置继续使用，自动导航参数与原点以工控机为准。显式保存时先备份再迁移，不会在启动时改写文件。");
}

void ConfigManager::applyJsonObjectToCurrentConfig(const QJsonObject &root)
{
    if (const auto object = root.value("manualControl").toObject(); !object.isEmpty()) {
        m_manualControl.maxLinearSpeed = object.value("maxLinearSpeed").toDouble(m_manualControl.maxLinearSpeed);
        m_manualControl.maxAngularSpeed = object.value("maxAngularSpeed").toDouble(m_manualControl.maxAngularSpeed);
        m_manualControl.manualMotionRepeatIntervalMs = object.value("manualMotionRepeatIntervalMs").toInt(m_manualControl.manualMotionRepeatIntervalMs);
    }
    if (const auto object = root.value("poseSource").toObject(); !object.isEmpty()) {
        m_poseSource.enabled = object.value("enabled").toBool(m_poseSource.enabled);
        m_poseSource.baseUrl = object.value("baseUrl").toString(m_poseSource.baseUrl);
        m_poseSource.requestTimeoutMs = object.value("requestTimeoutMs").toInt(m_poseSource.requestTimeoutMs);
    }
    if (const auto object = root.value("tracking").toObject(); !object.isEmpty()) {
        m_tracking.enabled = object.value("enabled").toBool(m_tracking.enabled);
        m_tracking.baseUrl = object.value("baseUrl").toString(m_tracking.baseUrl);
        m_tracking.requestTimeoutMs = object.value("requestTimeoutMs").toInt(m_tracking.requestTimeoutMs);
    }
    if (root.value("schemaVersion").toInt(1) < 2) {
        const auto legacy = root.value("control").toObject();
        m_manualControl.maxLinearSpeed = legacy.value("maxLinearSpeed").toDouble(m_manualControl.maxLinearSpeed);
        m_manualControl.maxAngularSpeed = legacy.value("maxAngularSpeed").toDouble(m_manualControl.maxAngularSpeed);
        m_manualControl.manualMotionRepeatIntervalMs = legacy.value("manualMotionRepeatIntervalMs").toInt(m_manualControl.manualMotionRepeatIntervalMs);
    }

    if (const QJsonObject routeObj = root.value(QStringLiteral("routePlanning")).toObject(); !routeObj.isEmpty()) {
        m_routePlanning.minEdgeCost =
            routeObj.value(QStringLiteral("minEdgeCost")).toDouble(m_routePlanning.minEdgeCost);
        m_routePlanning.edgePenalty =
            routeObj.value(QStringLiteral("edgePenalty")).toDouble(m_routePlanning.edgePenalty);
        m_routePlanning.arcPenalty =
            routeObj.value(QStringLiteral("arcPenalty")).toDouble(m_routePlanning.arcPenalty);
    }


    if (const QJsonObject videoObj = root.value(QStringLiteral("video")).toObject(); !videoObj.isEmpty()) {
        m_video.backend = videoObj.value(QStringLiteral("backend")).toString(m_video.backend).trimmed();
        m_video.streamUrl = videoObj.value(QStringLiteral("streamUrl")).toString(m_video.streamUrl);
        m_video.controlBaseUrl = videoObj.value(QStringLiteral("controlBaseUrl")).toString(m_video.controlBaseUrl).trimmed();
        m_video.deviceId = videoObj.value(QStringLiteral("deviceId")).toString(m_video.deviceId).trimmed();
        m_video.previewWidth = videoObj.value(QStringLiteral("previewWidth")).toInt(m_video.previewWidth);
        m_video.previewHeight = videoObj.value(QStringLiteral("previewHeight")).toInt(m_video.previewHeight);
        m_video.previewFps = videoObj.value(QStringLiteral("previewFps")).toInt(m_video.previewFps);
        m_video.recordMode = videoObj.value(QStringLiteral("recordMode")).toString(m_video.recordMode).trimmed();
        m_video.recordCodec = videoObj.value(QStringLiteral("recordCodec")).toString(m_video.recordCodec).trimmed();
        m_video.reconnectIntervalMs = videoObj.value(QStringLiteral("reconnectIntervalMs")).toInt(m_video.reconnectIntervalMs);
        m_video.reconnectIntervalMs = qMax(200, m_video.reconnectIntervalMs);
        m_video.cameraRequestTimeoutMs =
            videoObj.value(QStringLiteral("cameraRequestTimeoutMs")).toInt(m_video.cameraRequestTimeoutMs);
        m_video.autoStart = videoObj.value(QStringLiteral("autoStart")).toBool(m_video.autoStart);
        m_video.scaleContents = videoObj.value(QStringLiteral("scaleContents")).toBool(m_video.scaleContents);
    }

    if (const QJsonObject networkObj = root.value(QStringLiteral("network")).toObject(); !networkObj.isEmpty()) {
        m_network.websocketUrl = networkObj.value(QStringLiteral("websocketUrl")).toString(m_network.websocketUrl);
        m_network.statusReadUrl = networkObj.value(QStringLiteral("statusReadUrl")).toString(m_network.statusReadUrl);
        m_network.authToken = networkObj.value(QStringLiteral("authToken")).toString(m_network.authToken).trimmed();
        m_network.statusPollIntervalMs =
            networkObj.value(QStringLiteral("statusPollIntervalMs")).toInt(m_network.statusPollIntervalMs);
        m_network.statusPollIntervalMs = qBound(kMinStatusPollIntervalMs, m_network.statusPollIntervalMs, kMaxStatusPollIntervalMs);
        m_network.statusRequestTimeoutMs =
            networkObj.value(QStringLiteral("statusRequestTimeoutMs")).toInt(m_network.statusRequestTimeoutMs);
        m_network.statusMaxBackoffMs =
            networkObj.value(QStringLiteral("statusMaxBackoffMs")).toInt(m_network.statusMaxBackoffMs);
        m_network.chassisAutoReconnect =
            networkObj.value(QStringLiteral("chassisAutoReconnect")).toBool(m_network.chassisAutoReconnect);
        m_network.chassisReconnectIntervalMs =
            networkObj.value(QStringLiteral("chassisReconnectIntervalMs")).toInt(m_network.chassisReconnectIntervalMs);
        m_network.chassisReconnectMaxIntervalMs =
            networkObj.value(QStringLiteral("chassisReconnectMaxIntervalMs")).toInt(m_network.chassisReconnectMaxIntervalMs);
    }

    if (const QJsonObject gimbalObj = root.value(QStringLiteral("gimbal")).toObject(); !gimbalObj.isEmpty()) {
        m_gimbal.enabled = gimbalObj.value(QStringLiteral("enabled")).toBool(m_gimbal.enabled);
        m_gimbal.plcHost = gimbalObj.value(QStringLiteral("plcHost")).toString(m_gimbal.plcHost).trimmed();
        m_gimbal.plcPort = gimbalObj.value(QStringLiteral("plcPort")).toInt(m_gimbal.plcPort);
        m_gimbal.unitId = gimbalObj.value(QStringLiteral("unitId")).toInt(m_gimbal.unitId);
        m_gimbal.requestTimeoutMs = gimbalObj.value(QStringLiteral("requestTimeoutMs")).toInt(m_gimbal.requestTimeoutMs);
        m_gimbal.statusPollIntervalMs = gimbalObj.value(QStringLiteral("statusPollIntervalMs")).toInt(m_gimbal.statusPollIntervalMs);
        m_gimbal.heightControlAddress =
            gimbalObj.value(QStringLiteral("heightControlAddress")).toInt(m_gimbal.heightControlAddress);
        m_gimbal.pitchControlAddress =
            gimbalObj.value(QStringLiteral("pitchControlAddress")).toInt(m_gimbal.pitchControlAddress);
        m_gimbal.yawControlAddress =
            gimbalObj.value(QStringLiteral("yawControlAddress")).toInt(m_gimbal.yawControlAddress);
        m_gimbal.statusStartAddress =
            gimbalObj.value(QStringLiteral("statusStartAddress")).toInt(m_gimbal.statusStartAddress);
        m_gimbal.statusRegisterCount =
            gimbalObj.value(QStringLiteral("statusRegisterCount")).toInt(m_gimbal.statusRegisterCount);
        m_gimbal.minHeight = gimbalObj.value(QStringLiteral("minHeight")).toInt(m_gimbal.minHeight);
        m_gimbal.maxHeight = gimbalObj.value(QStringLiteral("maxHeight")).toInt(m_gimbal.maxHeight);
        m_gimbal.minYaw = gimbalObj.value(QStringLiteral("minYaw")).toInt(m_gimbal.minYaw);
        m_gimbal.maxYaw = gimbalObj.value(QStringLiteral("maxYaw")).toInt(m_gimbal.maxYaw);
        m_gimbal.minPitch = gimbalObj.value(QStringLiteral("minPitch")).toInt(m_gimbal.minPitch);
        m_gimbal.maxPitch = gimbalObj.value(QStringLiteral("maxPitch")).toInt(m_gimbal.maxPitch);
        m_gimbal.safetyStopTimeoutMs =
            gimbalObj.value(QStringLiteral("safetyStopTimeoutMs")).toInt(m_gimbal.safetyStopTimeoutMs);
    }

    if (const QJsonObject loggingObj = root.value(QStringLiteral("logging")).toObject(); !loggingObj.isEmpty()) {
        m_logging.level = loggingObj.value(QStringLiteral("level")).toString(m_logging.level).trimmed().toLower();
        m_logging.consoleEnabled = loggingObj.value(QStringLiteral("consoleEnabled")).toBool(m_logging.consoleEnabled);
        m_logging.fileEnabled = loggingObj.value(QStringLiteral("fileEnabled")).toBool(m_logging.fileEnabled);
        m_logging.includeSourceLocation =
            loggingObj.value(QStringLiteral("includeSourceLocation")).toBool(m_logging.includeSourceLocation);
        m_logging.includeThreadId = loggingObj.value(QStringLiteral("includeThreadId")).toBool(m_logging.includeThreadId);
        m_logging.includeCategory = loggingObj.value(QStringLiteral("includeCategory")).toBool(m_logging.includeCategory);
        m_logging.maxFileBytes =
            static_cast<qint64>(loggingObj.value(QStringLiteral("maxFileBytes")).toDouble(m_logging.maxFileBytes));
        m_logging.maxBackupFiles = loggingObj.value(QStringLiteral("maxBackupFiles")).toInt(m_logging.maxBackupFiles);
        m_logging.perSessionFile = loggingObj.value(QStringLiteral("perSessionFile")).toBool(m_logging.perSessionFile);
        m_logging.auditEnabled = loggingObj.value(QStringLiteral("auditEnabled")).toBool(m_logging.auditEnabled);
        m_logging.auditMaxFileBytes =
            static_cast<qint64>(loggingObj.value(QStringLiteral("auditMaxFileBytes")).toDouble(m_logging.auditMaxFileBytes));
        m_logging.auditMaxBackupFiles =
            loggingObj.value(QStringLiteral("auditMaxBackupFiles")).toInt(m_logging.auditMaxBackupFiles);
        m_logging.redactSensitiveData =
            loggingObj.value(QStringLiteral("redactSensitiveData")).toBool(m_logging.redactSensitiveData);
        m_logging.categoryRules.clear();
        const QJsonArray rules = loggingObj.value(QStringLiteral("categoryRules")).toArray();
        for (const QJsonValue &value : rules) {
            const QString rule = value.toString().trimmed();
            if (!rule.isEmpty()) {
                m_logging.categoryRules.push_back(rule);
            }
        }
    }

    if (const QJsonObject databaseObj = root.value(QStringLiteral("database")).toObject(); !databaseObj.isEmpty()) {
        m_database.backend = databaseObj.value(QStringLiteral("backend")).toString(m_database.backend).trimmed().toLower();
        m_database.connectionName =
            databaseObj.value(QStringLiteral("connectionName")).toString(m_database.connectionName).trimmed();
        m_database.sqliteFilePath =
            databaseObj.value(QStringLiteral("sqliteFilePath")).toString(m_database.sqliteFilePath).trimmed();
        m_database.host = databaseObj.value(QStringLiteral("host")).toString(m_database.host).trimmed();
        m_database.port = databaseObj.value(QStringLiteral("port")).toInt(m_database.port);
        m_database.databaseName =
            databaseObj.value(QStringLiteral("databaseName")).toString(m_database.databaseName).trimmed();
        m_database.userName = databaseObj.value(QStringLiteral("userName")).toString(m_database.userName).trimmed();
        m_database.password = databaseObj.value(QStringLiteral("password")).toString(m_database.password);
        m_database.useWAL = databaseObj.value(QStringLiteral("useWAL")).toBool(m_database.useWAL);
        m_database.foreignKeys = databaseObj.value(QStringLiteral("foreignKeys")).toBool(m_database.foreignKeys);
        m_database.synchronousNormal =
            databaseObj.value(QStringLiteral("synchronousNormal")).toBool(m_database.synchronousNormal);
        m_database.busyTimeoutMs =
            databaseObj.value(QStringLiteral("busyTimeoutMs")).toInt(m_database.busyTimeoutMs);
        m_database.cacheSizePages =
            databaseObj.value(QStringLiteral("cacheSizePages")).toInt(m_database.cacheSizePages);
        m_database.connectTimeoutMs =
            databaseObj.value(QStringLiteral("connectTimeoutMs")).toInt(m_database.connectTimeoutMs);
        m_database.reconnectIntervalMs =
            databaseObj.value(QStringLiteral("reconnectIntervalMs")).toInt(m_database.reconnectIntervalMs);
        m_database.enableTelemetryTables =
            databaseObj.value(QStringLiteral("enableTelemetryTables")).toBool(m_database.enableTelemetryTables);
        m_database.enableLocalCache =
            databaseObj.value(QStringLiteral("enableLocalCache")).toBool(m_database.enableLocalCache);
        m_database.enableAuditSync =
            databaseObj.value(QStringLiteral("enableAuditSync")).toBool(m_database.enableAuditSync);
    }
}

QStringList ConfigManager::validateJsonObject(const QJsonObject &root) const
{
    QStringList warnings;
    const auto retired = retiredLocalSections(root);
    if (!retired.isEmpty())
        warnings << tr("旧本机配置段 %1 已停用，不参与原点或自动导航计算。原点、融合及导航参数仅在工控机维护；显式保存时先保留原字节备份，再清除这些旧字段。").arg(retired.join(QStringLiteral("、")));
    const QStringList knownSections{
        QStringLiteral("geo"),
        QStringLiteral("schemaVersion"),
        QStringLiteral("manualControl"),
        QStringLiteral("taskDefaults"),
        QStringLiteral("poseSource"),
        QStringLiteral("tracking"),
        QStringLiteral("control"),
        QStringLiteral("routePlanning"),
        QStringLiteral("vehicle"),
        QStringLiteral("video"),
        QStringLiteral("network"),
        QStringLiteral("rowWork"),
        QStringLiteral("gimbal"),
        QStringLiteral("logging"),
        QStringLiteral("database"),
    };

    for (auto it = root.constBegin(); it != root.constEnd(); ++it) {
        if (!knownSections.contains(it.key())) {
            warnings << tr("配置包含未知顶层字段：%1").arg(it.key());
        }
    }

    const auto sectionObject = [&](const QString &section) -> QJsonObject {
        if (!root.contains(section)) {
            return {};
        }
        const QJsonValue value = root.value(section);
        if (!value.isObject()) {
            warnings << tr("配置段 %1 应为 JSON 对象").arg(section);
            return {};
        }
        return value.toObject();
    };

    const auto warnIfNotNumber = [&](const QJsonObject &object, const QString &section, const QString &key) {
        if (object.contains(key) && !object.value(key).isDouble()) {
            warnings << tr("配置项 %1.%2 应为数字").arg(section, key);
        }
    };

    const auto warnIfNotBool = [&](const QJsonObject &object, const QString &section, const QString &key) {
        if (object.contains(key) && !object.value(key).isBool()) {
            warnings << tr("配置项 %1.%2 应为布尔值").arg(section, key);
        }
    };

    const auto warnIfNotString = [&](const QJsonObject &object, const QString &section, const QString &key) {
        if (object.contains(key) && !object.value(key).isString()) {
            warnings << tr("配置项 %1.%2 应为字符串").arg(section, key);
        }
    };

    const auto warnIfInvalidUrl = [&](const QJsonObject &object,
                                      const QString &section,
                                      const QString &key,
                                      const QStringList &schemes) {
        if (!object.contains(key)) {
            return;
        }
        if (!object.value(key).isString()) {
            warnings << tr("配置项 %1.%2 应为 URL 字符串").arg(section, key);
            return;
        }
        const QString value = object.value(key).toString().trimmed();
        if (value.isEmpty()) {
            return;
        }
        const QUrl url(value);
        if (!url.isValid() || !schemes.contains(url.scheme())) {
            warnings << tr("配置项 %1.%2 URL 无效：%3").arg(section, key, value);
        }
    };

    warnIfNotNumber(sectionObject("manualControl"), "manualControl", "maxLinearSpeed");
    warnIfNotNumber(sectionObject("manualControl"), "manualControl", "maxAngularSpeed");
    warnIfNotNumber(sectionObject("manualControl"), "manualControl", "manualMotionRepeatIntervalMs");
    warnIfNotNumber(sectionObject("poseSource"), "poseSource", "requestTimeoutMs");
    warnIfNotNumber(sectionObject("tracking"), "tracking", "requestTimeoutMs");
    for (const auto &section : {QStringLiteral("poseSource"), QStringLiteral("tracking")}) {
        warnIfNotBool(sectionObject(section), section, "enabled");
        warnIfInvalidUrl(sectionObject(section), section, "baseUrl", {"http", "https"});
    }

    const QJsonObject routeObj = sectionObject(QStringLiteral("routePlanning"));
    warnIfNotNumber(routeObj, QStringLiteral("routePlanning"), QStringLiteral("minEdgeCost"));
    warnIfNotNumber(routeObj, QStringLiteral("routePlanning"), QStringLiteral("edgePenalty"));
    warnIfNotNumber(routeObj, QStringLiteral("routePlanning"), QStringLiteral("arcPenalty"));

    const QJsonObject videoObj = sectionObject(QStringLiteral("video"));
    warnIfInvalidUrl(videoObj, QStringLiteral("video"), QStringLiteral("streamUrl"), {QStringLiteral("http"), QStringLiteral("https")});
    warnIfInvalidUrl(videoObj, QStringLiteral("video"), QStringLiteral("controlBaseUrl"), {QStringLiteral("http"), QStringLiteral("https")});
    warnIfNotBool(videoObj, QStringLiteral("video"), QStringLiteral("autoStart"));
    warnIfNotBool(videoObj, QStringLiteral("video"), QStringLiteral("scaleContents"));

    const QJsonObject networkObj = sectionObject(QStringLiteral("network"));
    warnIfInvalidUrl(networkObj, QStringLiteral("network"), QStringLiteral("websocketUrl"), {QStringLiteral("ws"), QStringLiteral("wss")});
    warnIfInvalidUrl(networkObj, QStringLiteral("network"), QStringLiteral("statusReadUrl"), {QStringLiteral("http"), QStringLiteral("https")});
    warnIfNotString(networkObj, QStringLiteral("network"), QStringLiteral("authToken"));
    warnIfNotBool(networkObj, QStringLiteral("network"), QStringLiteral("chassisAutoReconnect"));

    const QJsonObject loggingObj = sectionObject(QStringLiteral("logging"));
    warnIfNotString(loggingObj, QStringLiteral("logging"), QStringLiteral("level"));
    warnIfNotBool(loggingObj, QStringLiteral("logging"), QStringLiteral("consoleEnabled"));
    warnIfNotBool(loggingObj, QStringLiteral("logging"), QStringLiteral("fileEnabled"));
    warnIfNotBool(loggingObj, QStringLiteral("logging"), QStringLiteral("auditEnabled"));
    warnIfNotBool(loggingObj, QStringLiteral("logging"), QStringLiteral("redactSensitiveData"));

    const QJsonObject databaseObj = sectionObject(QStringLiteral("database"));
    warnIfNotString(databaseObj, QStringLiteral("database"), QStringLiteral("backend"));
    warnIfNotString(databaseObj, QStringLiteral("database"), QStringLiteral("password"));

    return warnings;
}

void ConfigManager::applyEnvironmentOverrides()
{
    const QString authToken = qEnvironmentVariable("TENCO_AUTH_TOKEN").trimmed();
    if (!authToken.isEmpty()) {
        m_network.authToken = authToken;
        qCInfo(lcConfigManager) << "Network auth token loaded from TENCO_AUTH_TOKEN";
    }

    const QString databasePassword = qEnvironmentVariable("TENCO_DATABASE_PASSWORD");
    if (!databasePassword.isEmpty()) {
        m_database.password = databasePassword;
        qCInfo(lcConfigManager) << "Database password loaded from TENCO_DATABASE_PASSWORD";
    }
}

bool ConfigManager::saveToFile(const QString &path, QString *errorMessage) const
{
    if (path.trimmed().isEmpty()) {
        if (errorMessage) {
            *errorMessage = tr("配置文件路径为空");
        }
        return false;
    }

    const QFileInfo fileInfo(path);
    const QDir dir = fileInfo.absoluteDir();
    if (!dir.exists() && !QDir().mkpath(dir.absolutePath())) {
        if (errorMessage) {
            *errorMessage = tr("无法创建配置目录：%1").arg(dir.absolutePath());
        }
        return false;
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (errorMessage) {
            *errorMessage = tr("无法写入配置文件：%1").arg(file.errorString());
        }
        return false;
    }

    const QJsonDocument doc(toJsonObject());
    const auto bytes = doc.toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size()) { if (errorMessage) *errorMessage = tr("配置写入不完整，原文件保留"); return false; }
    if (!file.commit()) {
        if (errorMessage) {
            *errorMessage = tr("保存配置文件失败：%1").arg(file.errorString());
        }
        return false;
    }

    return true;
}

bool ConfigManager::writeFileAtomically(const QByteArray &bytes, const QString &targetPath, QString *errorMessage) const
{
    const QFileInfo targetInfo(targetPath);
    const QDir targetDir = targetInfo.absoluteDir();
    if (!targetDir.exists() && !QDir().mkpath(targetDir.absolutePath())) {
        if (errorMessage) {
            *errorMessage = tr("无法创建目标配置目录：%1").arg(targetDir.absolutePath());
        }
        return false;
    }

    QSaveFile target(targetPath);
    if (!target.open(QIODevice::WriteOnly)) {
        if (errorMessage) {
            *errorMessage = tr("无法写入目标配置文件：%1").arg(target.errorString());
        }
        return false;
    }
    if (target.write(bytes) != bytes.size()) { if (errorMessage) *errorMessage = tr("配置复制不完整，原文件保留"); return false; }
    if (!target.commit()) {
        if (errorMessage) {
            *errorMessage = tr("保存目标配置文件失败：%1").arg(target.errorString());
        }
        return false;
    }
    return true;
}

QJsonObject ConfigManager::toJsonObject() const
{
    QJsonObject root;
    root.insert("schemaVersion", 2);
    root.insert("manualControl", QJsonObject{{"maxLinearSpeed", m_manualControl.maxLinearSpeed},{"maxAngularSpeed", m_manualControl.maxAngularSpeed},{"manualMotionRepeatIntervalMs", m_manualControl.manualMotionRepeatIntervalMs}});
    root.insert("poseSource", QJsonObject{{"enabled", m_poseSource.enabled},{"baseUrl", m_poseSource.baseUrl},{"requestTimeoutMs", m_poseSource.requestTimeoutMs}});
    root.insert("tracking", QJsonObject{{"enabled", m_tracking.enabled},{"baseUrl", m_tracking.baseUrl},{"requestTimeoutMs", m_tracking.requestTimeoutMs}});

    root.insert(QStringLiteral("routePlanning"),
                QJsonObject{
                    {QStringLiteral("minEdgeCost"), m_routePlanning.minEdgeCost},
                    {QStringLiteral("edgePenalty"), m_routePlanning.edgePenalty},
                    {QStringLiteral("arcPenalty"), m_routePlanning.arcPenalty},
                });



    root.insert(QStringLiteral("video"),
                QJsonObject{
                    {QStringLiteral("backend"), m_video.backend},
                    {QStringLiteral("streamUrl"), m_video.streamUrl},
                    {QStringLiteral("controlBaseUrl"), m_video.controlBaseUrl},
                    {QStringLiteral("deviceId"), m_video.deviceId},
                    {QStringLiteral("previewWidth"), m_video.previewWidth},
                    {QStringLiteral("previewHeight"), m_video.previewHeight},
                    {QStringLiteral("previewFps"), m_video.previewFps},
                    {QStringLiteral("recordMode"), m_video.recordMode},
                    {QStringLiteral("recordCodec"), m_video.recordCodec},
                    {QStringLiteral("reconnectIntervalMs"), m_video.reconnectIntervalMs},
                    {QStringLiteral("cameraRequestTimeoutMs"), m_video.cameraRequestTimeoutMs},
                    {QStringLiteral("autoStart"), m_video.autoStart},
                    {QStringLiteral("scaleContents"), m_video.scaleContents},
                });

    root.insert(QStringLiteral("network"),
                QJsonObject{
                    {QStringLiteral("websocketUrl"), m_network.websocketUrl},
                    {QStringLiteral("statusReadUrl"), m_network.statusReadUrl},
                    {QStringLiteral("authToken"), m_network.authToken},
                    {QStringLiteral("statusPollIntervalMs"), m_network.statusPollIntervalMs},
                    {QStringLiteral("statusRequestTimeoutMs"), m_network.statusRequestTimeoutMs},
                    {QStringLiteral("statusMaxBackoffMs"), m_network.statusMaxBackoffMs},
                    {QStringLiteral("chassisAutoReconnect"), m_network.chassisAutoReconnect},
                    {QStringLiteral("chassisReconnectIntervalMs"), m_network.chassisReconnectIntervalMs},
                    {QStringLiteral("chassisReconnectMaxIntervalMs"), m_network.chassisReconnectMaxIntervalMs},
                });

    root.insert(QStringLiteral("gimbal"),
                QJsonObject{
                    {QStringLiteral("enabled"), m_gimbal.enabled},
                    {QStringLiteral("plcHost"), m_gimbal.plcHost},
                    {QStringLiteral("plcPort"), m_gimbal.plcPort},
                    {QStringLiteral("unitId"), m_gimbal.unitId},
                    {QStringLiteral("requestTimeoutMs"), m_gimbal.requestTimeoutMs},
                    {QStringLiteral("statusPollIntervalMs"), m_gimbal.statusPollIntervalMs},
                    {QStringLiteral("heightControlAddress"), m_gimbal.heightControlAddress},
                    {QStringLiteral("pitchControlAddress"), m_gimbal.pitchControlAddress},
                    {QStringLiteral("yawControlAddress"), m_gimbal.yawControlAddress},
                    {QStringLiteral("statusStartAddress"), m_gimbal.statusStartAddress},
                    {QStringLiteral("statusRegisterCount"), m_gimbal.statusRegisterCount},
                    {QStringLiteral("minHeight"), m_gimbal.minHeight},
                    {QStringLiteral("maxHeight"), m_gimbal.maxHeight},
                    {QStringLiteral("minYaw"), m_gimbal.minYaw},
                    {QStringLiteral("maxYaw"), m_gimbal.maxYaw},
                    {QStringLiteral("minPitch"), m_gimbal.minPitch},
                    {QStringLiteral("maxPitch"), m_gimbal.maxPitch},
                    {QStringLiteral("safetyStopTimeoutMs"), m_gimbal.safetyStopTimeoutMs},
                });

    QJsonArray categoryRules;
    for (const QString &rule : m_logging.categoryRules) {
        categoryRules.append(rule);
    }
    root.insert(QStringLiteral("logging"),
                QJsonObject{
                    {QStringLiteral("level"), m_logging.level},
                    {QStringLiteral("consoleEnabled"), m_logging.consoleEnabled},
                    {QStringLiteral("fileEnabled"), m_logging.fileEnabled},
                    {QStringLiteral("includeSourceLocation"), m_logging.includeSourceLocation},
                    {QStringLiteral("includeThreadId"), m_logging.includeThreadId},
                    {QStringLiteral("includeCategory"), m_logging.includeCategory},
                    {QStringLiteral("maxFileBytes"), static_cast<double>(m_logging.maxFileBytes)},
                    {QStringLiteral("maxBackupFiles"), m_logging.maxBackupFiles},
                    {QStringLiteral("perSessionFile"), m_logging.perSessionFile},
                    {QStringLiteral("auditEnabled"), m_logging.auditEnabled},
                    {QStringLiteral("auditMaxFileBytes"), static_cast<double>(m_logging.auditMaxFileBytes)},
                    {QStringLiteral("auditMaxBackupFiles"), m_logging.auditMaxBackupFiles},
                    {QStringLiteral("redactSensitiveData"), m_logging.redactSensitiveData},
                    {QStringLiteral("categoryRules"), categoryRules},
                });

    root.insert(QStringLiteral("database"),
                QJsonObject{
                    {QStringLiteral("backend"), m_database.backend},
                    {QStringLiteral("connectionName"), m_database.connectionName},
                    {QStringLiteral("sqliteFilePath"), m_database.sqliteFilePath},
                    {QStringLiteral("host"), m_database.host},
                    {QStringLiteral("port"), m_database.port},
                    {QStringLiteral("databaseName"), m_database.databaseName},
                    {QStringLiteral("userName"), m_database.userName},
                    {QStringLiteral("password"), m_database.password},
                    {QStringLiteral("useWAL"), m_database.useWAL},
                    {QStringLiteral("foreignKeys"), m_database.foreignKeys},
                    {QStringLiteral("synchronousNormal"), m_database.synchronousNormal},
                    {QStringLiteral("busyTimeoutMs"), m_database.busyTimeoutMs},
                    {QStringLiteral("cacheSizePages"), m_database.cacheSizePages},
                    {QStringLiteral("connectTimeoutMs"), m_database.connectTimeoutMs},
                    {QStringLiteral("reconnectIntervalMs"), m_database.reconnectIntervalMs},
                    {QStringLiteral("enableTelemetryTables"), m_database.enableTelemetryTables},
                    {QStringLiteral("enableLocalCache"), m_database.enableLocalCache},
                    {QStringLiteral("enableAuditSync"), m_database.enableAuditSync},
                });

    return retireLocalNavigationSettings(mergeDocuments(m_sourceDocument, root));
}

void ConfigManager::loadDefaults()
{
    m_manualControl = ManualControlConfig{};
    m_poseSource = PoseSourceConfig{};
    m_tracking = TrackingConfig{};
    m_routePlanning = RoutePlanningConfig{};
    m_routePlanning.minEdgeCost = kDefaultRouteMinEdgeCost;
    m_routePlanning.edgePenalty = kDefaultRouteEdgePenalty;
    m_routePlanning.arcPenalty = kDefaultRouteArcPenalty;
    m_video = VideoConfig{};
    m_video.backend = QString::fromUtf8(kDefaultVideoBackend);
    m_video.recordMode = QString::fromUtf8(kDefaultVideoRecordMode);
    m_video.recordCodec = QString::fromUtf8(kDefaultVideoRecordCodec);
    m_video.cameraRequestTimeoutMs = kDefaultCameraRequestTimeoutMs;
    m_network = NetworkConfig{};
    m_network.websocketUrl = QString::fromUtf8(kDefaultWebSocketUrl);
    m_network.statusReadUrl = QString::fromUtf8(kDefaultStatusReadUrl);
    m_network.statusPollIntervalMs = kDefaultStatusPollIntervalMs;
    m_network.statusRequestTimeoutMs = kDefaultStatusRequestTimeoutMs;
    m_network.statusMaxBackoffMs = kDefaultStatusMaxBackoffMs;
    m_network.chassisReconnectIntervalMs = kDefaultChassisReconnectIntervalMs;
    m_network.chassisReconnectMaxIntervalMs = kDefaultChassisReconnectMaxIntervalMs;
    m_gimbal = GimbalConfig{};
    m_gimbal.plcHost = QString::fromUtf8(kDefaultGimbalPlcHost);
    m_gimbal.plcPort = kDefaultGimbalPlcPort;
    m_gimbal.unitId = kDefaultGimbalUnitId;
    m_gimbal.requestTimeoutMs = kDefaultGimbalRequestTimeoutMs;
    m_gimbal.statusPollIntervalMs = kDefaultGimbalStatusPollIntervalMs;
    m_gimbal.safetyStopTimeoutMs = kDefaultGimbalSafetyStopTimeoutMs;
    m_logging = LoggingConfig{};
#ifdef QT_DEBUG
    m_logging.level = QStringLiteral("debug");
#else
    m_logging.level = QStringLiteral("info");
#endif
    m_logging.maxFileBytes = kDefaultLogMaxFileBytes;
    m_logging.maxBackupFiles = kDefaultLogMaxBackupFiles;
    m_logging.auditMaxFileBytes = kDefaultLogMaxFileBytes;
    m_logging.auditMaxBackupFiles = kDefaultLogMaxBackupFiles;
    m_logging.categoryRules = {
        QStringLiteral("tenco.net.status.debug=false"),
        QStringLiteral("tenco.gimbal.control.debug=false")
    };

    m_database = DatabaseConfig{};
    m_database.backend = QString::fromUtf8(kDefaultDatabaseBackend);
    m_database.connectionName = QString::fromUtf8(kDefaultDatabaseConnectionName);
    m_database.databaseName = QString::fromUtf8(kDefaultDatabaseName);
    m_database.port = kDefaultDatabasePort;
    m_database.busyTimeoutMs = kDefaultDatabaseBusyTimeoutMs;
    m_database.cacheSizePages = kDefaultDatabaseCacheSizePages;
    m_database.connectTimeoutMs = kDefaultDatabaseConnectTimeoutMs;
    m_database.reconnectIntervalMs = kDefaultDatabaseReconnectIntervalMs;
    m_database.useWAL = true;
    m_database.foreignKeys = true;
    m_database.synchronousNormal = true;
    m_database.enableTelemetryTables = true;
    m_database.enableLocalCache = true;
    m_database.enableAuditSync = true;
}

void ConfigManager::sanitizeConfig()
{
    m_manualControl.maxLinearSpeed = std::isfinite(m_manualControl.maxLinearSpeed)
        ? qBound(0.0, m_manualControl.maxLinearSpeed, VehicleParameters::maxOperatingSpeedMps)
        : VehicleParameters::maxOperatingSpeedMps;
    if (m_manualControl.maxLinearSpeed > 0 &&
        m_manualControl.maxLinearSpeed < VehicleParameters::minOperatingSpeedMps)
        m_manualControl.maxLinearSpeed = VehicleParameters::minOperatingSpeedMps;
    m_manualControl.maxAngularSpeed = qBound(0.0, m_manualControl.maxAngularSpeed, 5.0);
    m_manualControl.manualMotionRepeatIntervalMs = qBound(20, m_manualControl.manualMotionRepeatIntervalMs, 100);

    m_poseSource.requestTimeoutMs = qBound(500, m_poseSource.requestTimeoutMs, 10000);
    m_tracking.requestTimeoutMs = qBound(500, m_tracking.requestTimeoutMs, 10000);

    m_routePlanning.minEdgeCost = qBound(1e-9, m_routePlanning.minEdgeCost, 1000000.0);
    m_routePlanning.edgePenalty = qBound(0.0, m_routePlanning.edgePenalty, 1000000.0);
    m_routePlanning.arcPenalty = qBound(0.0, m_routePlanning.arcPenalty, 1000000.0);


    m_video.reconnectIntervalMs = qMax(200, m_video.reconnectIntervalMs);
    m_video.cameraRequestTimeoutMs = qBound(1000, m_video.cameraRequestTimeoutMs, 30000);
    m_video.deviceId = m_video.deviceId.trimmed();
    if (m_video.backend.isEmpty()) {
        m_video.backend = QString::fromUtf8(kDefaultVideoBackend);
    }
    if (m_video.backend != QStringLiteral("mjpeg_http") && m_video.backend != QStringLiteral("oak_depthai")) {
        qCWarning(lcConfigManager) << "Unsupported video backend, fallback to mjpeg_http:" << m_video.backend;
        m_video.backend = QString::fromUtf8(kDefaultVideoBackend);
    }
    m_video.previewWidth = qBound(320, m_video.previewWidth, 4096);
    m_video.previewHeight = qBound(240, m_video.previewHeight, 3040);
    m_video.previewFps = qBound(1, m_video.previewFps, 120);
    m_video.controlBaseUrl = m_video.controlBaseUrl.trimmed();
    if (m_video.recordMode.isEmpty()) {
        m_video.recordMode = QString::fromUtf8(kDefaultVideoRecordMode);
    }
    if (m_video.recordMode != QStringLiteral("host_opencv")) {
        qCWarning(lcConfigManager) << "Unsupported video record mode, fallback to host_opencv:" << m_video.recordMode;
        m_video.recordMode = QString::fromUtf8(kDefaultVideoRecordMode);
    }
    if (m_video.recordCodec.isEmpty()) {
        m_video.recordCodec = QString::fromUtf8(kDefaultVideoRecordCodec);
    }
    static const QStringList supportedCodecs{
        QStringLiteral("MJPG"),
        QStringLiteral("XVID"),
        QStringLiteral("MP4V")
    };
    m_video.recordCodec = m_video.recordCodec.trimmed().toUpper();
    if (!supportedCodecs.contains(m_video.recordCodec)) {
        qCWarning(lcConfigManager) << "Unsupported video record codec, fallback to MJPG:" << m_video.recordCodec;
        m_video.recordCodec = QString::fromUtf8(kDefaultVideoRecordCodec);
    }
    m_network.statusPollIntervalMs = qBound(kMinStatusPollIntervalMs, m_network.statusPollIntervalMs, kMaxStatusPollIntervalMs);
    m_network.statusRequestTimeoutMs = qBound(500, m_network.statusRequestTimeoutMs, 30000);
    m_network.statusMaxBackoffMs = qBound(m_network.statusPollIntervalMs, m_network.statusMaxBackoffMs, 60000);
    m_network.chassisReconnectIntervalMs = qBound(200, m_network.chassisReconnectIntervalMs, 60000);
    m_network.chassisReconnectMaxIntervalMs =
        qBound(m_network.chassisReconnectIntervalMs, m_network.chassisReconnectMaxIntervalMs, 120000);

    m_gimbal.plcHost = m_gimbal.plcHost.trimmed();
    m_gimbal.plcPort = qBound(1, m_gimbal.plcPort, 65535);
    m_gimbal.unitId = qBound(1, m_gimbal.unitId, 255);
    m_gimbal.requestTimeoutMs = qBound(200, m_gimbal.requestTimeoutMs, 10000);
    m_gimbal.statusPollIntervalMs = qBound(100, m_gimbal.statusPollIntervalMs, 10000);
    m_gimbal.heightControlAddress = qMax(0, m_gimbal.heightControlAddress);
    m_gimbal.pitchControlAddress = qMax(0, m_gimbal.pitchControlAddress);
    m_gimbal.yawControlAddress = qMax(0, m_gimbal.yawControlAddress);
    m_gimbal.statusStartAddress = qMax(0, m_gimbal.statusStartAddress);
    m_gimbal.statusRegisterCount = qMax(7, m_gimbal.statusRegisterCount);
    m_gimbal.safetyStopTimeoutMs = qBound(300, m_gimbal.safetyStopTimeoutMs, 30000);
    if (m_gimbal.minHeight > m_gimbal.maxHeight) {
        std::swap(m_gimbal.minHeight, m_gimbal.maxHeight);
    }
    if (m_gimbal.minYaw > m_gimbal.maxYaw) {
        std::swap(m_gimbal.minYaw, m_gimbal.maxYaw);
    }
    if (m_gimbal.minPitch > m_gimbal.maxPitch) {
        std::swap(m_gimbal.minPitch, m_gimbal.maxPitch);
    }

    m_logging.level = m_logging.level.trimmed().toLower();
    if (m_logging.level != QStringLiteral("debug") &&
        m_logging.level != QStringLiteral("info") &&
        m_logging.level != QStringLiteral("warn") &&
        m_logging.level != QStringLiteral("error") &&
        m_logging.level != QStringLiteral("fatal")) {
        m_logging.level = QStringLiteral("info");
    }
    m_logging.maxFileBytes = qBound(kMinLogMaxFileBytes, m_logging.maxFileBytes, kMaxLogMaxFileBytes);
    m_logging.maxBackupFiles = qBound(1, m_logging.maxBackupFiles, 99);
    m_logging.auditMaxFileBytes = qBound(kMinLogMaxFileBytes, m_logging.auditMaxFileBytes, kMaxLogMaxFileBytes);
    m_logging.auditMaxBackupFiles = qBound(1, m_logging.auditMaxBackupFiles, 99);
    QStringList sanitizedRules;
    for (const QString &rule : std::as_const(m_logging.categoryRules)) {
        const QString trimmed = rule.trimmed();
        if (!trimmed.isEmpty()) {
            sanitizedRules.push_back(trimmed);
        }
    }
    m_logging.categoryRules = sanitizedRules;

    m_database.backend = m_database.backend.trimmed().toLower();
    if (m_database.backend.isEmpty()) {
        m_database.backend = QString::fromUtf8(kDefaultDatabaseBackend);
    }
    if (m_database.backend != QStringLiteral("sqlite") &&
        m_database.backend != QStringLiteral("postgresql")) {
        qCWarning(lcConfigManager) << "Unsupported database backend, fallback to sqlite:" << m_database.backend;
        m_database.backend = QString::fromUtf8(kDefaultDatabaseBackend);
    }
    m_database.connectionName = m_database.connectionName.trimmed();
    if (m_database.connectionName.isEmpty()) {
        m_database.connectionName = QString::fromUtf8(kDefaultDatabaseConnectionName);
    }
    m_database.sqliteFilePath = m_database.sqliteFilePath.trimmed();
    m_database.host = m_database.host.trimmed();
    m_database.databaseName = m_database.databaseName.trimmed();
    if (m_database.databaseName.isEmpty()) {
        m_database.databaseName = QString::fromUtf8(kDefaultDatabaseName);
    }
    m_database.userName = m_database.userName.trimmed();
    m_database.password = m_database.password.trimmed();
    m_database.port = qBound(1, m_database.port, 65535);
    m_database.useWAL = m_database.useWAL;
    m_database.foreignKeys = m_database.foreignKeys;
    m_database.synchronousNormal = m_database.synchronousNormal;
    m_database.busyTimeoutMs = qBound(100, m_database.busyTimeoutMs, 30000);
    m_database.cacheSizePages = qBound(64, m_database.cacheSizePages, 262144);
    m_database.connectTimeoutMs = qBound(500, m_database.connectTimeoutMs, 30000);
    m_database.reconnectIntervalMs = qBound(500, m_database.reconnectIntervalMs, 60000);
}
