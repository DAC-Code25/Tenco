#ifndef CONFIGMANAGER_H
#define CONFIGMANAGER_H

#include <QObject>
#include <QJsonObject>
#include <QList>
#include <QPointF>
#include <QString>
#include <QStringList>

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
        int routeFollowerUpdateIntervalMs = 100;
        int manualMotionRepeatIntervalMs = 40;
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
        int cameraRequestTimeoutMs = 5000;
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
        int statusRequestTimeoutMs = 3000;
        int statusMaxBackoffMs = 5000;
        bool chassisAutoReconnect = true;
        int chassisReconnectIntervalMs = 1000;
        int chassisReconnectMaxIntervalMs = 15000;
    };

    struct RowWorkConfig {
        bool enabled = false;
        QString gatewayBaseUrl;
        int statusPollIntervalMs = 300;
        int commandTimeoutMs = 3000;
        bool autoRefreshPlanStatus = true;
    };

    struct GimbalConfig {
        bool enabled = false;
        QString plcHost;
        int plcPort = 502;
        int unitId = 255;
        int requestTimeoutMs = 1000;
        int statusPollIntervalMs = 300;
        int heightControlAddress = 0;
        int pitchControlAddress = 1;
        int yawControlAddress = 2;
        int statusStartAddress = 100;
        int statusRegisterCount = 7;
        int minHeight = -1900;
        int maxHeight = 6000;
        int minYaw = 5;
        int maxYaw = 175;
        int minPitch = 95;
        int maxPitch = 265;
        int safetyStopTimeoutMs = 1500;
    };

    struct LoggingConfig {
        QString level = QStringLiteral("info");
        bool consoleEnabled = true;
        bool fileEnabled = true;
        bool includeSourceLocation = false;
        bool includeThreadId = true;
        bool includeCategory = true;
        qint64 maxFileBytes = 5 * 1024 * 1024;
        int maxBackupFiles = 10;
        bool perSessionFile = true;
        bool auditEnabled = true;
        qint64 auditMaxFileBytes = 5 * 1024 * 1024;
        int auditMaxBackupFiles = 10;
        bool redactSensitiveData = true;
        QStringList categoryRules;
    };

    struct ConfigSnapshot {
        GeoConfig geo;
        ControlConfig control;
        VehicleConfig vehicle;
        VideoConfig video;
        NetworkConfig network;
        RowWorkConfig rowWork;
        GimbalConfig gimbal;
        LoggingConfig logging;
    };

    static ConfigManager &instance();

    void reload();
    void setConfigFilePath(const QString &path);
    ConfigSnapshot snapshot() const;
    bool saveSnapshot(const ConfigSnapshot &snapshot, QString *errorMessage = nullptr, bool applyAfterSave = false);
    bool createStartupBackup(QString *errorMessage = nullptr);
    QString backupDirectoryPath() const;
    QStringList backupFilePaths() const;
    QString latestBackupFilePath() const;
    bool currentConfigMatchesLatestBackup() const;
    bool loadSnapshotFromFile(const QString &path, ConfigSnapshot *snapshot, QString *errorMessage = nullptr);
    bool restoreLatestBackup(QString *errorMessage = nullptr, bool applyAfterRestore = true);

    const GeoConfig &geo() const { return m_geo; }
    const ControlConfig &control() const { return m_control; }
    const VehicleConfig &vehicle() const { return m_vehicle; }
    const VideoConfig &video() const { return m_video; }
    const NetworkConfig &network() const { return m_network; }
    const RowWorkConfig &rowWork() const { return m_rowWork; }
    const GimbalConfig &gimbal() const { return m_gimbal; }
    const LoggingConfig &logging() const { return m_logging; }

    QPointF geoToLocal(double latitudeDeg, double longitudeDeg) const;
    void localToGeo(const QPointF &localPoint, double &latitudeDeg, double &longitudeDeg) const;

    QString configFilePath() const { return m_configPath; }
    bool loadedFromFile() const { return m_loadedFromFile; }

signals:
    void configChanged();

private:
    explicit ConfigManager(QObject *parent = nullptr);
    void ensureLoaded();
    void load();
    void loadFromFile(const QString &path);
    void applyJsonObjectToCurrentConfig(const QJsonObject &root);
    bool saveToFile(const QString &path, QString *errorMessage = nullptr) const;
    bool copyFileAtomically(const QString &sourcePath, const QString &targetPath, QString *errorMessage = nullptr) const;
    void pruneBackups(int keepCount);
    void loadDefaults();
    void sanitizeConfig();
    void updateCachedScales();
    QJsonObject toJsonObject() const;

    GeoConfig m_geo;
    ControlConfig m_control;
    VehicleConfig m_vehicle;
    VideoConfig m_video;
    NetworkConfig m_network;
    RowWorkConfig m_rowWork;
    GimbalConfig m_gimbal;
    LoggingConfig m_logging;
    double m_metersPerDegLat = 0.0;
    double m_metersPerDegLon = 0.0;
    QString m_configPath;
    QString m_configPathOverride;
    bool m_loaded = false;
    bool m_loadedFromFile = false;
    bool m_startupBackupCreated = false;
};

#endif // CONFIGMANAGER_H
