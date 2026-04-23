#include "configmanager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QStringList>
#include <QtGlobal>
#include <QtMath>

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
constexpr int kMinStatusPollIntervalMs = 50;
constexpr int kMaxStatusPollIntervalMs = 5000;

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
}

void ConfigManager::setConfigFilePath(const QString &path)
{
    m_configPathOverride = path.trimmed();
    reload();
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
        qCWarning(lcConfigManager) << "Config file not found, fallback to defaults:" << path;
    }
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
        qCWarning(lcConfigManager) << "Invalid JSON object in config:" << path;
        return;
    }

    const QJsonObject root = doc.object();

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
        m_video.deviceId = videoObj.value(QStringLiteral("deviceId")).toString(m_video.deviceId).trimmed();
        m_video.previewWidth = videoObj.value(QStringLiteral("previewWidth")).toInt(m_video.previewWidth);
        m_video.previewHeight = videoObj.value(QStringLiteral("previewHeight")).toInt(m_video.previewHeight);
        m_video.previewFps = videoObj.value(QStringLiteral("previewFps")).toInt(m_video.previewFps);
        m_video.recordMode = videoObj.value(QStringLiteral("recordMode")).toString(m_video.recordMode).trimmed();
        m_video.recordCodec = videoObj.value(QStringLiteral("recordCodec")).toString(m_video.recordCodec).trimmed();
        m_video.reconnectIntervalMs = videoObj.value(QStringLiteral("reconnectIntervalMs")).toInt(m_video.reconnectIntervalMs);
        m_video.reconnectIntervalMs = qMax(200, m_video.reconnectIntervalMs);
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
    }
}

void ConfigManager::loadDefaults()
{
    m_geo.baseLatitudeDeg = kDefaultLatDeg;
    m_geo.baseLongitudeDeg = kDefaultLonDeg;
    m_control = ControlConfig{};
    m_vehicle = VehicleConfig{};
    m_video = VideoConfig{};
    m_video.backend = QString::fromUtf8(kDefaultVideoBackend);
    m_video.recordMode = QString::fromUtf8(kDefaultVideoRecordMode);
    m_video.recordCodec = QString::fromUtf8(kDefaultVideoRecordCodec);
    m_network = NetworkConfig{};
    m_network.websocketUrl = QString::fromUtf8(kDefaultWebSocketUrl);
    m_network.statusReadUrl = QString::fromUtf8(kDefaultStatusReadUrl);
    m_network.writeInsUrl = QString::fromUtf8(kDefaultWriteInsUrl);
    m_network.saveFileUrl = QString::fromUtf8(kDefaultSaveFileUrl);
    m_network.statusPollIntervalMs = kDefaultStatusPollIntervalMs;
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

    m_vehicle.wheelBaseMeters = qMax(0.01, m_vehicle.wheelBaseMeters);
    m_vehicle.wheelDiameterMeters = qMax(0.01, m_vehicle.wheelDiameterMeters);
    m_vehicle.gearReduction = qMax(0.01, m_vehicle.gearReduction);

    m_video.reconnectIntervalMs = qMax(200, m_video.reconnectIntervalMs);
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
