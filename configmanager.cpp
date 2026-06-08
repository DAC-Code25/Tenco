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
#include <QStringList>
#include <QUrl>
#include <QtGlobal>
#include <QtMath>
#include <utility>

Q_LOGGING_CATEGORY(lcConfigManager, "tenco.config")

namespace {
constexpr double kEarthRadiusMeters = 6378137.0; // WGS84 equatorial radius
constexpr double kDefaultLatDeg = 0.0;
constexpr double kDefaultLonDeg = 0.0;

constexpr const char *kDefaultWebSocketUrl = "ws://192.168.31.7:1202";
constexpr const char *kDefaultStatusReadUrl = "http://192.168.31.7:9999/table/reads";
constexpr const char *kDefaultWriteInsUrl = "http://192.168.31.7:9999/table/writeIns";
constexpr const char *kDefaultSaveFileUrl = "http://192.168.31.7:9999/saveFile";
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
constexpr const char *kDefaultRowWorkGatewayBaseUrl = "http://192.168.31.13:18120";
constexpr int kDefaultRowWorkStatusPollIntervalMs = 300;
constexpr int kDefaultRowWorkCommandTimeoutMs = 3000;
constexpr int kMinRowWorkStatusPollIntervalMs = 100;
constexpr int kMaxRowWorkStatusPollIntervalMs = 10000;
constexpr int kMinRowWorkCommandTimeoutMs = 1000;
constexpr int kMaxRowWorkCommandTimeoutMs = 20000;
constexpr const char *kDefaultGimbalPlcHost = "192.168.31.120";
constexpr int kDefaultGimbalPlcPort = 502;
constexpr int kDefaultGimbalUnitId = 255;
constexpr int kDefaultGimbalRequestTimeoutMs = 1000;
constexpr int kDefaultGimbalStatusPollIntervalMs = 300;
constexpr int kDefaultCameraRequestTimeoutMs = 5000;
constexpr int kDefaultRouteFollowerUpdateIntervalMs = 100;
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
constexpr int kMaxConfigBackups = 10;

QString defaultConfigPath()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString exeConfig = QDir(appDir).filePath(QStringLiteral("config.json"));
    if (QFile::exists(exeConfig)) {
        return exeConfig;
    }

    const QString cwdConfig = QDir::current().absoluteFilePath(QStringLiteral("config.json"));
    if (QFile::exists(cwdConfig)) {
        return cwdConfig;
    }

    const QString parentConfig = QDir(appDir).filePath(QStringLiteral("../config.json"));
    if (QFile::exists(parentConfig)) {
        return parentConfig;
    }

    const QString grandParentConfig = QDir(appDir).filePath(QStringLiteral("../../config.json"));
    if (QFile::exists(grandParentConfig)) {
        return grandParentConfig;
    }

    return exeConfig;
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
    m_configPathOverride = path.trimmed();
    reload();
}

ConfigManager::ConfigSnapshot ConfigManager::snapshot() const
{
    ConfigSnapshot snap;
    snap.geo = m_geo;
    snap.control = m_control;
    snap.routePlanning = m_routePlanning;
    snap.vehicle = m_vehicle;
    snap.video = m_video;
    snap.network = m_network;
    snap.rowWork = m_rowWork;
    snap.gimbal = m_gimbal;
    snap.logging = m_logging;
    snap.database = m_database;
    return snap;
}

bool ConfigManager::saveSnapshot(const ConfigSnapshot &snapshot, QString *errorMessage, bool applyAfterSave)
{
    const ConfigSnapshot oldSnapshot = this->snapshot();
    const QString oldPath = m_configPath;
    const bool oldLoaded = m_loaded;
    const bool oldLoadedFromFile = m_loadedFromFile;
    const double oldMetersPerDegLat = m_metersPerDegLat;
    const double oldMetersPerDegLon = m_metersPerDegLon;

    m_geo = snapshot.geo;
    m_control = snapshot.control;
    m_routePlanning = snapshot.routePlanning;
    m_vehicle = snapshot.vehicle;
    m_video = snapshot.video;
    m_network = snapshot.network;
    m_rowWork = snapshot.rowWork;
    m_gimbal = snapshot.gimbal;
    m_logging = snapshot.logging;
    m_database = snapshot.database;
    sanitizeConfig();
    updateCachedScales();

    const QString targetPath = m_configPath.isEmpty()
                                   ? (m_configPathOverride.isEmpty() ? defaultConfigPath() : m_configPathOverride)
                                   : m_configPath;
    if (!saveToFile(targetPath, errorMessage)) {
        m_geo = oldSnapshot.geo;
        m_control = oldSnapshot.control;
        m_routePlanning = oldSnapshot.routePlanning;
        m_vehicle = oldSnapshot.vehicle;
        m_video = oldSnapshot.video;
        m_network = oldSnapshot.network;
        m_rowWork = oldSnapshot.rowWork;
        m_gimbal = oldSnapshot.gimbal;
        m_logging = oldSnapshot.logging;
        m_database = oldSnapshot.database;
        m_configPath = oldPath;
        m_loaded = oldLoaded;
        m_loadedFromFile = oldLoadedFromFile;
        m_metersPerDegLat = oldMetersPerDegLat;
        m_metersPerDegLon = oldMetersPerDegLon;
        return false;
    }

    if (applyAfterSave) {
        m_configPath = targetPath;
        m_loaded = true;
        m_loadedFromFile = true;
        emit configChanged();
    } else {
        m_geo = oldSnapshot.geo;
        m_control = oldSnapshot.control;
        m_routePlanning = oldSnapshot.routePlanning;
        m_vehicle = oldSnapshot.vehicle;
        m_video = oldSnapshot.video;
        m_network = oldSnapshot.network;
        m_rowWork = oldSnapshot.rowWork;
        m_gimbal = oldSnapshot.gimbal;
        m_logging = oldSnapshot.logging;
        m_database = oldSnapshot.database;
        m_configPath = oldPath;
        m_loaded = oldLoaded;
        m_loadedFromFile = oldLoadedFromFile;
        m_metersPerDegLat = oldMetersPerDegLat;
        m_metersPerDegLon = oldMetersPerDegLon;
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

    const QDir backupDir(backupDirectoryPath());
    if (!backupDir.exists() && !QDir().mkpath(backupDir.absolutePath())) {
        if (errorMessage) {
            *errorMessage = tr("无法创建配置备份目录：%1").arg(backupDir.absolutePath());
        }
        return false;
    }

    pruneBackups(kMaxConfigBackups - 1);

    const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"));
    const QString targetPath = backupDir.filePath(QStringLiteral("config_%1.json").arg(timestamp));
    if (!copyFileAtomically(m_configPath, targetPath, errorMessage)) {
        return false;
    }

    m_startupBackupCreated = true;
    pruneBackups(kMaxConfigBackups);
    return true;
}

QString ConfigManager::backupDirectoryPath() const
{
    const QFileInfo configInfo(m_configPath.isEmpty() ? defaultConfigPath() : m_configPath);
    return configInfo.absoluteDir().filePath(QStringLiteral("config_backups"));
}

QStringList ConfigManager::backupFilePaths() const
{
    const QDir dir(backupDirectoryPath());
    const QFileInfoList entries = dir.entryInfoList(QStringList{QStringLiteral("config_*.json")},
                                                    QDir::Files,
                                                    QDir::Time);
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

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) {
        if (errorMessage) {
            *errorMessage = tr("配置文件不是合法 JSON 对象：%1").arg(path);
        }
        return false;
    }

    const ConfigSnapshot oldSnapshot = this->snapshot();
    const double oldMetersPerDegLat = m_metersPerDegLat;
    const double oldMetersPerDegLon = m_metersPerDegLon;

    loadDefaults();
    applyJsonObjectToCurrentConfig(doc.object());
    sanitizeConfig();
    updateCachedScales();
    *snapshot = this->snapshot();

    m_geo = oldSnapshot.geo;
    m_control = oldSnapshot.control;
    m_routePlanning = oldSnapshot.routePlanning;
    m_vehicle = oldSnapshot.vehicle;
    m_video = oldSnapshot.video;
    m_network = oldSnapshot.network;
    m_rowWork = oldSnapshot.rowWork;
    m_gimbal = oldSnapshot.gimbal;
    m_logging = oldSnapshot.logging;
    m_database = oldSnapshot.database;
    m_metersPerDegLat = oldMetersPerDegLat;
    m_metersPerDegLon = oldMetersPerDegLon;
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

    if (!copyFileAtomically(latestPath, m_configPath, errorMessage)) {
        return false;
    }

    if (applyAfterRestore) {
        reload();
    }
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
    loadDefaults();
    if (QFile::exists(path)) {
        loadFromFile(path);
        m_loadedFromFile = true;
    } else {
        m_validationWarnings.clear();
        qCWarning(lcConfigManager) << "Config file not found, fallback to defaults:" << path;
    }
    applyEnvironmentOverrides();
    sanitizeConfig();
    m_loaded = true;
    updateCachedScales();
    qCInfo(lcConfigManager) << "Config loaded from" << m_configPath << ", from file?" << m_loadedFromFile;
}

void ConfigManager::loadFromFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        loadDefaults();
        return;
    }

    const QByteArray raw = file.readAll();
    file.close();

    const QJsonDocument doc = QJsonDocument::fromJson(raw);
    if (!doc.isObject()) {
        m_validationWarnings = {tr("配置文件不是合法 JSON 对象：%1").arg(path)};
        qCWarning(lcConfigManager) << "Invalid JSON object in config:" << path;
        return;
    }

    const QJsonObject root = doc.object();
    m_validationWarnings = validateJsonObject(root);
    for (const QString &warning : std::as_const(m_validationWarnings)) {
        qCWarning(lcConfigManager) << warning;
    }
    applyJsonObjectToCurrentConfig(root);
}

void ConfigManager::applyJsonObjectToCurrentConfig(const QJsonObject &root)
{
    if (const QJsonObject geoObj = root.value(QStringLiteral("geo")).toObject(); !geoObj.isEmpty()) {
        m_geo.baseLatitudeDeg = geoObj.value(QStringLiteral("baseLatitudeDeg")).toDouble(kDefaultLatDeg);
        m_geo.baseLongitudeDeg = geoObj.value(QStringLiteral("baseLongitudeDeg")).toDouble(kDefaultLonDeg);
    } else {
        m_geo = GeoConfig{};
    }

    if (const QJsonObject ctrlObj = root.value(QStringLiteral("control")).toObject(); !ctrlObj.isEmpty()) {
        m_control.arrivalDistanceThreshold = ctrlObj.value(QStringLiteral("arrivalDistanceThreshold")).toDouble(m_control.arrivalDistanceThreshold);
        m_control.arrivalAngleThresholdDeg = ctrlObj.value(QStringLiteral("arrivalAngleThresholdDeg")).toDouble(m_control.arrivalAngleThresholdDeg);
        m_control.maxLinearSpeed = ctrlObj.value(QStringLiteral("maxLinearSpeed")).toDouble(m_control.maxLinearSpeed);
        m_control.maxAngularSpeed = ctrlObj.value(QStringLiteral("maxAngularSpeed")).toDouble(m_control.maxAngularSpeed);
        m_control.linearGain = ctrlObj.value(QStringLiteral("linearGain")).toDouble(m_control.linearGain);
        m_control.angularGain = ctrlObj.value(QStringLiteral("angularGain")).toDouble(m_control.angularGain);
        m_control.headingStopThresholdDeg = ctrlObj.value(QStringLiteral("headingStopThresholdDeg")).toDouble(m_control.headingStopThresholdDeg);
        m_control.headingSlowdownThresholdDeg = ctrlObj.value(QStringLiteral("headingSlowdownThresholdDeg")).toDouble(m_control.headingSlowdownThresholdDeg);
        m_control.headingSlowdownFactor = ctrlObj.value(QStringLiteral("headingSlowdownFactor")).toDouble(m_control.headingSlowdownFactor);
        m_control.nearTargetDistanceMultiplier = ctrlObj.value(QStringLiteral("nearTargetDistanceMultiplier")).toDouble(m_control.nearTargetDistanceMultiplier);
        m_control.nearTargetSpeedMultiplier = ctrlObj.value(QStringLiteral("nearTargetSpeedMultiplier")).toDouble(m_control.nearTargetSpeedMultiplier);
        m_control.linearAccelerationLimit = ctrlObj.value(QStringLiteral("linearAccelerationLimit")).toDouble(m_control.linearAccelerationLimit);
        m_control.linearDecelerationLimit = ctrlObj.value(QStringLiteral("linearDecelerationLimit")).toDouble(m_control.linearDecelerationLimit);
        m_control.angularAccelerationLimit = ctrlObj.value(QStringLiteral("angularAccelerationLimit")).toDouble(m_control.angularAccelerationLimit);
        m_control.angularDecelerationLimit = ctrlObj.value(QStringLiteral("angularDecelerationLimit")).toDouble(m_control.angularDecelerationLimit);
        m_control.finalAdjustLinearSpeed = ctrlObj.value(QStringLiteral("finalAdjustLinearSpeed")).toDouble(m_control.finalAdjustLinearSpeed);
        m_control.finalAdjustAngularSpeed = ctrlObj.value(QStringLiteral("finalAdjustAngularSpeed")).toDouble(m_control.finalAdjustAngularSpeed);
        m_control.routeFollowerUpdateIntervalMs =
            ctrlObj.value(QStringLiteral("routeFollowerUpdateIntervalMs")).toInt(m_control.routeFollowerUpdateIntervalMs);
        m_control.manualMotionRepeatIntervalMs =
            ctrlObj.value(QStringLiteral("manualMotionRepeatIntervalMs")).toInt(m_control.manualMotionRepeatIntervalMs);
    }

    if (const QJsonObject routeObj = root.value(QStringLiteral("routePlanning")).toObject(); !routeObj.isEmpty()) {
        m_routePlanning.minEdgeCost =
            routeObj.value(QStringLiteral("minEdgeCost")).toDouble(m_routePlanning.minEdgeCost);
        m_routePlanning.edgePenalty =
            routeObj.value(QStringLiteral("edgePenalty")).toDouble(m_routePlanning.edgePenalty);
        m_routePlanning.arcPenalty =
            routeObj.value(QStringLiteral("arcPenalty")).toDouble(m_routePlanning.arcPenalty);
    }

    if (const QJsonObject vehicleObj = root.value(QStringLiteral("vehicle")).toObject(); !vehicleObj.isEmpty()) {
        m_vehicle.wheelBaseMeters = vehicleObj.value(QStringLiteral("wheelBaseMeters")).toDouble(m_vehicle.wheelBaseMeters);
        m_vehicle.wheelDiameterMeters = vehicleObj.value(QStringLiteral("wheelDiameterMeters")).toDouble(m_vehicle.wheelDiameterMeters);
        m_vehicle.gearReduction = vehicleObj.value(QStringLiteral("gearReduction")).toDouble(m_vehicle.gearReduction);
    }

    if (const QJsonObject videoObj = root.value(QStringLiteral("video")).toObject(); !videoObj.isEmpty()) {
        m_video.backend = videoObj.value(QStringLiteral("backend")).toString(m_video.backend).trimmed();
        m_video.streamUrl = videoObj.value(QStringLiteral("streamUrl")).toString(m_video.streamUrl);
        m_video.controlBaseUrl = videoObj.value(QStringLiteral("controlBaseUrl")).toString(m_video.controlBaseUrl).trimmed();
        m_video.streamOptions.clear();
        const QJsonArray streamOptions = videoObj.value(QStringLiteral("streamOptions")).toArray();
        for (const QJsonValue &value : streamOptions) {
            const QJsonObject optionObj = value.toObject();
            if (optionObj.isEmpty()) {
                continue;
            }
            VideoConfig::StreamOption option;
            option.name = optionObj.value(QStringLiteral("name")).toString().trimmed();
            option.url = optionObj.value(QStringLiteral("url")).toString().trimmed();
            m_video.streamOptions.push_back(option);
        }
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
        m_network.writeInsUrl = networkObj.value(QStringLiteral("writeInsUrl")).toString(m_network.writeInsUrl);
        m_network.saveFileUrl = networkObj.value(QStringLiteral("saveFileUrl")).toString(m_network.saveFileUrl);
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

    if (const QJsonObject rowWorkObj = root.value(QStringLiteral("rowWork")).toObject(); !rowWorkObj.isEmpty()) {
        m_rowWork.enabled = rowWorkObj.value(QStringLiteral("enabled")).toBool(m_rowWork.enabled);
        m_rowWork.gatewayBaseUrl = rowWorkObj.value(QStringLiteral("gatewayBaseUrl")).toString(m_rowWork.gatewayBaseUrl).trimmed();
        m_rowWork.statusPollIntervalMs =
            rowWorkObj.value(QStringLiteral("statusPollIntervalMs")).toInt(m_rowWork.statusPollIntervalMs);
        m_rowWork.commandTimeoutMs =
            rowWorkObj.value(QStringLiteral("commandTimeoutMs")).toInt(m_rowWork.commandTimeoutMs);
        m_rowWork.autoRefreshPlanStatus =
            rowWorkObj.value(QStringLiteral("autoRefreshPlanStatus")).toBool(m_rowWork.autoRefreshPlanStatus);
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
    const QStringList knownSections{
        QStringLiteral("geo"),
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

    const QJsonObject controlObj = sectionObject(QStringLiteral("control"));
    const QStringList controlNumbers{
        QStringLiteral("arrivalDistanceThreshold"),
        QStringLiteral("arrivalAngleThresholdDeg"),
        QStringLiteral("maxLinearSpeed"),
        QStringLiteral("maxAngularSpeed"),
        QStringLiteral("linearGain"),
        QStringLiteral("angularGain"),
        QStringLiteral("headingStopThresholdDeg"),
        QStringLiteral("headingSlowdownThresholdDeg"),
        QStringLiteral("headingSlowdownFactor"),
        QStringLiteral("nearTargetDistanceMultiplier"),
        QStringLiteral("nearTargetSpeedMultiplier"),
        QStringLiteral("linearAccelerationLimit"),
        QStringLiteral("linearDecelerationLimit"),
        QStringLiteral("angularAccelerationLimit"),
        QStringLiteral("angularDecelerationLimit"),
        QStringLiteral("finalAdjustLinearSpeed"),
        QStringLiteral("finalAdjustAngularSpeed"),
        QStringLiteral("routeFollowerUpdateIntervalMs"),
        QStringLiteral("manualMotionRepeatIntervalMs"),
    };
    for (const QString &key : controlNumbers) {
        warnIfNotNumber(controlObj, QStringLiteral("control"), key);
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
    warnIfInvalidUrl(networkObj, QStringLiteral("network"), QStringLiteral("writeInsUrl"), {QStringLiteral("http"), QStringLiteral("https")});
    warnIfInvalidUrl(networkObj, QStringLiteral("network"), QStringLiteral("saveFileUrl"), {QStringLiteral("http"), QStringLiteral("https")});
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
    file.write(doc.toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (errorMessage) {
            *errorMessage = tr("保存配置文件失败：%1").arg(file.errorString());
        }
        return false;
    }

    return true;
}

bool ConfigManager::copyFileAtomically(const QString &sourcePath, const QString &targetPath, QString *errorMessage) const
{
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        if (errorMessage) {
            *errorMessage = tr("无法读取源配置文件：%1").arg(source.errorString());
        }
        return false;
    }

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
    target.write(source.readAll());
    if (!target.commit()) {
        if (errorMessage) {
            *errorMessage = tr("保存目标配置文件失败：%1").arg(target.errorString());
        }
        return false;
    }
    return true;
}

void ConfigManager::pruneBackups(int keepCount)
{
    const QDir dir(backupDirectoryPath());
    const QFileInfoList entries = dir.entryInfoList(QStringList{QStringLiteral("config_*.json")},
                                                    QDir::Files,
                                                    QDir::Time);
    for (int i = qMax(keepCount, 0); i < entries.size(); ++i) {
        QFile::remove(entries.at(i).absoluteFilePath());
    }
}

QJsonObject ConfigManager::toJsonObject() const
{
    QJsonObject root;
    root.insert(QStringLiteral("geo"), QJsonObject{
                                           {QStringLiteral("baseLatitudeDeg"), m_geo.baseLatitudeDeg},
                                           {QStringLiteral("baseLongitudeDeg"), m_geo.baseLongitudeDeg},
                                       });

    root.insert(QStringLiteral("control"),
                QJsonObject{
                    {QStringLiteral("arrivalDistanceThreshold"), m_control.arrivalDistanceThreshold},
                    {QStringLiteral("arrivalAngleThresholdDeg"), m_control.arrivalAngleThresholdDeg},
                    {QStringLiteral("maxLinearSpeed"), m_control.maxLinearSpeed},
                    {QStringLiteral("maxAngularSpeed"), m_control.maxAngularSpeed},
                    {QStringLiteral("linearGain"), m_control.linearGain},
                    {QStringLiteral("angularGain"), m_control.angularGain},
                    {QStringLiteral("headingStopThresholdDeg"), m_control.headingStopThresholdDeg},
                    {QStringLiteral("headingSlowdownThresholdDeg"), m_control.headingSlowdownThresholdDeg},
                    {QStringLiteral("headingSlowdownFactor"), m_control.headingSlowdownFactor},
                    {QStringLiteral("nearTargetDistanceMultiplier"), m_control.nearTargetDistanceMultiplier},
                    {QStringLiteral("nearTargetSpeedMultiplier"), m_control.nearTargetSpeedMultiplier},
                    {QStringLiteral("linearAccelerationLimit"), m_control.linearAccelerationLimit},
                    {QStringLiteral("linearDecelerationLimit"), m_control.linearDecelerationLimit},
                    {QStringLiteral("angularAccelerationLimit"), m_control.angularAccelerationLimit},
                    {QStringLiteral("angularDecelerationLimit"), m_control.angularDecelerationLimit},
                    {QStringLiteral("finalAdjustLinearSpeed"), m_control.finalAdjustLinearSpeed},
                    {QStringLiteral("finalAdjustAngularSpeed"), m_control.finalAdjustAngularSpeed},
                    {QStringLiteral("routeFollowerUpdateIntervalMs"), m_control.routeFollowerUpdateIntervalMs},
                    {QStringLiteral("manualMotionRepeatIntervalMs"), m_control.manualMotionRepeatIntervalMs},
                });

    root.insert(QStringLiteral("routePlanning"),
                QJsonObject{
                    {QStringLiteral("minEdgeCost"), m_routePlanning.minEdgeCost},
                    {QStringLiteral("edgePenalty"), m_routePlanning.edgePenalty},
                    {QStringLiteral("arcPenalty"), m_routePlanning.arcPenalty},
                });

    root.insert(QStringLiteral("vehicle"),
                QJsonObject{
                    {QStringLiteral("wheelBaseMeters"), m_vehicle.wheelBaseMeters},
                    {QStringLiteral("wheelDiameterMeters"), m_vehicle.wheelDiameterMeters},
                    {QStringLiteral("gearReduction"), m_vehicle.gearReduction},
                });

    QJsonArray streamOptions;
    for (const VideoConfig::StreamOption &option : m_video.streamOptions) {
        streamOptions.append(QJsonObject{
            {QStringLiteral("name"), option.name},
            {QStringLiteral("url"), option.url},
        });
    }
    root.insert(QStringLiteral("video"),
                QJsonObject{
                    {QStringLiteral("backend"), m_video.backend},
                    {QStringLiteral("streamUrl"), m_video.streamUrl},
                    {QStringLiteral("controlBaseUrl"), m_video.controlBaseUrl},
                    {QStringLiteral("streamOptions"), streamOptions},
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
                    {QStringLiteral("writeInsUrl"), m_network.writeInsUrl},
                    {QStringLiteral("saveFileUrl"), m_network.saveFileUrl},
                    {QStringLiteral("authToken"), m_network.authToken},
                    {QStringLiteral("statusPollIntervalMs"), m_network.statusPollIntervalMs},
                    {QStringLiteral("statusRequestTimeoutMs"), m_network.statusRequestTimeoutMs},
                    {QStringLiteral("statusMaxBackoffMs"), m_network.statusMaxBackoffMs},
                    {QStringLiteral("chassisAutoReconnect"), m_network.chassisAutoReconnect},
                    {QStringLiteral("chassisReconnectIntervalMs"), m_network.chassisReconnectIntervalMs},
                    {QStringLiteral("chassisReconnectMaxIntervalMs"), m_network.chassisReconnectMaxIntervalMs},
                });

    root.insert(QStringLiteral("rowWork"),
                QJsonObject{
                    {QStringLiteral("enabled"), m_rowWork.enabled},
                    {QStringLiteral("gatewayBaseUrl"), m_rowWork.gatewayBaseUrl},
                    {QStringLiteral("statusPollIntervalMs"), m_rowWork.statusPollIntervalMs},
                    {QStringLiteral("commandTimeoutMs"), m_rowWork.commandTimeoutMs},
                    {QStringLiteral("autoRefreshPlanStatus"), m_rowWork.autoRefreshPlanStatus},
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

    return root;
}

void ConfigManager::loadDefaults()
{
    m_geo.baseLatitudeDeg = kDefaultLatDeg;
    m_geo.baseLongitudeDeg = kDefaultLonDeg;
    m_control = ControlConfig{};
    m_control.routeFollowerUpdateIntervalMs = kDefaultRouteFollowerUpdateIntervalMs;
    m_control.manualMotionRepeatIntervalMs = kDefaultManualMotionRepeatIntervalMs;
    m_routePlanning = RoutePlanningConfig{};
    m_routePlanning.minEdgeCost = kDefaultRouteMinEdgeCost;
    m_routePlanning.edgePenalty = kDefaultRouteEdgePenalty;
    m_routePlanning.arcPenalty = kDefaultRouteArcPenalty;
    m_vehicle = VehicleConfig{};
    m_video = VideoConfig{};
    m_video.backend = QString::fromUtf8(kDefaultVideoBackend);
    m_video.recordMode = QString::fromUtf8(kDefaultVideoRecordMode);
    m_video.recordCodec = QString::fromUtf8(kDefaultVideoRecordCodec);
    m_video.cameraRequestTimeoutMs = kDefaultCameraRequestTimeoutMs;
    m_network = NetworkConfig{};
    m_network.websocketUrl = QString::fromUtf8(kDefaultWebSocketUrl);
    m_network.statusReadUrl = QString::fromUtf8(kDefaultStatusReadUrl);
    m_network.writeInsUrl = QString::fromUtf8(kDefaultWriteInsUrl);
    m_network.saveFileUrl = QString::fromUtf8(kDefaultSaveFileUrl);
    m_network.statusPollIntervalMs = kDefaultStatusPollIntervalMs;
    m_network.statusRequestTimeoutMs = kDefaultStatusRequestTimeoutMs;
    m_network.statusMaxBackoffMs = kDefaultStatusMaxBackoffMs;
    m_network.chassisReconnectIntervalMs = kDefaultChassisReconnectIntervalMs;
    m_network.chassisReconnectMaxIntervalMs = kDefaultChassisReconnectMaxIntervalMs;
    m_rowWork = RowWorkConfig{};
    m_rowWork.gatewayBaseUrl = QString::fromUtf8(kDefaultRowWorkGatewayBaseUrl);
    m_rowWork.statusPollIntervalMs = kDefaultRowWorkStatusPollIntervalMs;
    m_rowWork.commandTimeoutMs = kDefaultRowWorkCommandTimeoutMs;
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
    m_control.arrivalDistanceThreshold = qMax(0.01, m_control.arrivalDistanceThreshold);
    m_control.arrivalAngleThresholdDeg = qBound(0.0, m_control.arrivalAngleThresholdDeg, 180.0);
    m_control.maxLinearSpeed = qMax(0.0, m_control.maxLinearSpeed);
    m_control.maxAngularSpeed = qMax(0.0, m_control.maxAngularSpeed);
    m_control.linearGain = qMax(0.0, m_control.linearGain);
    m_control.angularGain = qMax(0.0, m_control.angularGain);
    m_control.headingStopThresholdDeg = qBound(0.0, m_control.headingStopThresholdDeg, 180.0);
    m_control.headingSlowdownThresholdDeg = qBound(0.0, m_control.headingSlowdownThresholdDeg, 180.0);
    m_control.headingSlowdownFactor = qBound(0.0, m_control.headingSlowdownFactor, 1.0);
    m_control.nearTargetDistanceMultiplier = qMax(1.0, m_control.nearTargetDistanceMultiplier);
    m_control.nearTargetSpeedMultiplier = qBound(0.0, m_control.nearTargetSpeedMultiplier, 1.0);
    m_control.linearAccelerationLimit = qMax(0.0, m_control.linearAccelerationLimit);
    m_control.linearDecelerationLimit = qMax(0.0, m_control.linearDecelerationLimit);
    m_control.angularAccelerationLimit = qMax(0.0, m_control.angularAccelerationLimit);
    m_control.angularDecelerationLimit = qMax(0.0, m_control.angularDecelerationLimit);
    m_control.finalAdjustLinearSpeed = qMax(0.0, m_control.finalAdjustLinearSpeed);
    m_control.finalAdjustAngularSpeed = qMax(0.0, m_control.finalAdjustAngularSpeed);
    m_control.routeFollowerUpdateIntervalMs = qBound(20, m_control.routeFollowerUpdateIntervalMs, 1000);
    m_control.manualMotionRepeatIntervalMs = qBound(20, m_control.manualMotionRepeatIntervalMs, 1000);

    m_routePlanning.minEdgeCost = qBound(1e-9, m_routePlanning.minEdgeCost, 1000000.0);
    m_routePlanning.edgePenalty = qBound(0.0, m_routePlanning.edgePenalty, 1000000.0);
    m_routePlanning.arcPenalty = qBound(0.0, m_routePlanning.arcPenalty, 1000000.0);

    m_vehicle.wheelBaseMeters = qMax(0.01, m_vehicle.wheelBaseMeters);
    m_vehicle.wheelDiameterMeters = qMax(0.01, m_vehicle.wheelDiameterMeters);
    m_vehicle.gearReduction = qMax(0.01, m_vehicle.gearReduction);

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
    QList<VideoConfig::StreamOption> sanitizedStreamOptions;
    for (const VideoConfig::StreamOption &option : std::as_const(m_video.streamOptions)) {
        VideoConfig::StreamOption sanitizedOption = option;
        sanitizedOption.name = sanitizedOption.name.trimmed();
        sanitizedOption.url = sanitizedOption.url.trimmed();
        if (sanitizedOption.url.isEmpty()) {
            continue;
        }
        const QUrl optionUrl(sanitizedOption.url);
        if (!optionUrl.isValid()) {
            qCWarning(lcConfigManager) << "Ignoring invalid video stream option URL:" << sanitizedOption.url;
            continue;
        }
        if (sanitizedOption.name.isEmpty()) {
            sanitizedOption.name = sanitizedOption.url;
        }
        sanitizedStreamOptions.push_back(sanitizedOption);
    }
    m_video.streamOptions = sanitizedStreamOptions;
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

    m_rowWork.gatewayBaseUrl = m_rowWork.gatewayBaseUrl.trimmed();
    m_rowWork.statusPollIntervalMs =
        qBound(kMinRowWorkStatusPollIntervalMs, m_rowWork.statusPollIntervalMs, kMaxRowWorkStatusPollIntervalMs);
    m_rowWork.commandTimeoutMs =
        qBound(kMinRowWorkCommandTimeoutMs, m_rowWork.commandTimeoutMs, kMaxRowWorkCommandTimeoutMs);

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

void ConfigManager::updateCachedScales()
{
    const double latRad = qDegreesToRadians(m_geo.baseLatitudeDeg);
    m_metersPerDegLat = (M_PI / 180.0) * kEarthRadiusMeters;
    m_metersPerDegLon = m_metersPerDegLat * qCos(latRad);
    if (qFuzzyIsNull(m_metersPerDegLon)) {
        m_metersPerDegLon = m_metersPerDegLat * 1e-6;
    }
}

QPointF ConfigManager::geoToLocal(double latitudeDeg, double longitudeDeg) const
{
    const double dLat = latitudeDeg - m_geo.baseLatitudeDeg;
    const double dLon = longitudeDeg - m_geo.baseLongitudeDeg;
    const double x = dLon * m_metersPerDegLon;
    const double y = dLat * m_metersPerDegLat;
    return QPointF(x, y);
}

void ConfigManager::localToGeo(const QPointF &localPoint, double &latitudeDeg, double &longitudeDeg) const
{
    latitudeDeg = m_geo.baseLatitudeDeg + (localPoint.y() / m_metersPerDegLat);
    longitudeDeg = m_geo.baseLongitudeDeg + (localPoint.x() / m_metersPerDegLon);
}
