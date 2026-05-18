#ifndef CONFIGMANAGER_H
#define CONFIGMANAGER_H

#include <QObject>
#include <QList>
#include <QPointF>
#include <QString>

class ConfigManager : public QObject
{
    Q_OBJECT

public:
    struct GeoConfig {
        double baseLatitudeDeg = 0.0;
        double baseLongitudeDeg = 0.0;
    };

    struct ControlConfig {
        double arrivalDistanceThreshold = 0.2; // meters
        double arrivalAngleThresholdDeg = 5.0; // degrees
        double maxLinearSpeed = 0.5;          // m/s
        double maxAngularSpeed = 0.5;         // rad/s
        double linearGain = 0.8;
        double angularGain = 1.0;
        double headingStopThresholdDeg = 90.0;
        double headingSlowdownThresholdDeg = 45.0;
        double headingSlowdownFactor = 0.3;
        double nearTargetDistanceMultiplier = 3.0;
        double nearTargetSpeedMultiplier = 0.4;
        double linearAccelerationLimit = 0.6;   // m/s^2
        double linearDecelerationLimit = 0.8;   // m/s^2
        double angularAccelerationLimit = 1.2;  // rad/s^2
        double angularDecelerationLimit = 1.5;  // rad/s^2
        double finalAdjustLinearSpeed = 0.2;    // m/s
        double finalAdjustAngularSpeed = 0.6;   // rad/s
    };

    struct VehicleConfig {
        double wheelBaseMeters = 0.6;
        double wheelDiameterMeters = 0.2;
        double gearReduction = 1.0;
    };

    struct VideoConfig {
        struct StreamOption {
            QString name;
            QString url;
        };

        QString backend;
        QString streamUrl;
        QString controlBaseUrl;
        QList<StreamOption> streamOptions;
        QString deviceId;
        int previewWidth = 1280;
        int previewHeight = 720;
        int previewFps = 30;
        QString recordMode;
        QString recordCodec;
        int reconnectIntervalMs = 2000;
        bool autoStart = true;
        bool scaleContents = true;
    };

    struct NetworkConfig {
        QString websocketUrl;
        QString statusReadUrl;
        QString writeInsUrl;
        QString saveFileUrl;
        QString authToken;
        int statusPollIntervalMs = 100;
    };

    struct RowWorkConfig {
        bool enabled = false;
        QString gatewayBaseUrl;
        int statusPollIntervalMs = 300;
        int commandTimeoutMs = 3000;
        bool autoRefreshPlanStatus = true;
    };

    static ConfigManager &instance();

    void reload();
    void setConfigFilePath(const QString &path);

    const GeoConfig &geo() const { return m_geo; }
    const ControlConfig &control() const { return m_control; }
    const VehicleConfig &vehicle() const { return m_vehicle; }
    const VideoConfig &video() const { return m_video; }
    const NetworkConfig &network() const { return m_network; }
    const RowWorkConfig &rowWork() const { return m_rowWork; }

    QPointF geoToLocal(double latitudeDeg, double longitudeDeg) const;
    void localToGeo(const QPointF &localPoint, double &latitudeDeg, double &longitudeDeg) const;

    QString configFilePath() const { return m_configPath; }
    bool loadedFromFile() const { return m_loadedFromFile; }

private:
    explicit ConfigManager(QObject *parent = nullptr);
    void ensureLoaded();
    void load();
    void loadFromFile(const QString &path);
    void loadDefaults();
    void sanitizeConfig();
    void updateCachedScales();

    GeoConfig m_geo;
    ControlConfig m_control;
    VehicleConfig m_vehicle;
    VideoConfig m_video;
    NetworkConfig m_network;
    RowWorkConfig m_rowWork;
    double m_metersPerDegLat = 0.0;
    double m_metersPerDegLon = 0.0;
    QString m_configPath;
    QString m_configPathOverride;
    bool m_loaded = false;
    bool m_loadedFromFile = false;
};

#endif // CONFIGMANAGER_H
