#include "configmanager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtMath>

namespace {
constexpr double kEarthRadiusMeters = 6378137.0; // WGS84 equatorial radius
constexpr double kDefaultLatDeg = 0.0;
constexpr double kDefaultLonDeg = 0.0;

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
    const QString path = defaultConfigPath();
    m_configPath = path;
    loadDefaults();
    if (QFile::exists(path)) {
        loadFromFile(path);
    }
    m_loaded = true;
    updateCachedScales();
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
        m_video.streamUrl = videoObj.value(QStringLiteral("streamUrl")).toString(m_video.streamUrl);
        m_video.reconnectIntervalMs = videoObj.value(QStringLiteral("reconnectIntervalMs")).toInt(m_video.reconnectIntervalMs);
        m_video.reconnectIntervalMs = qMax(200, m_video.reconnectIntervalMs);
        m_video.autoStart = videoObj.value(QStringLiteral("autoStart")).toBool(m_video.autoStart);
        m_video.scaleContents = videoObj.value(QStringLiteral("scaleContents")).toBool(m_video.scaleContents);
    }
}

void ConfigManager::loadDefaults()
{
    m_geo.baseLatitudeDeg = kDefaultLatDeg;
    m_geo.baseLongitudeDeg = kDefaultLonDeg;
    m_control = ControlConfig{};
    m_vehicle = VehicleConfig{};
    m_video = VideoConfig{};
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
