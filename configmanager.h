#ifndef CONFIGMANAGER_H
#define CONFIGMANAGER_H
#include "vehicleparameters.h"

#include <QObject>
#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

class ConfigManager : public QObject
{
    Q_OBJECT

public:
    struct ManualControlConfig {
        double maxLinearSpeed = VehicleParameters::maxOperatingSpeedMps;
        double maxAngularSpeed = 0.5;
        int manualMotionRepeatIntervalMs = 40;
    };

    struct PoseSourceConfig {
        bool enabled = true;
        QString baseUrl = QStringLiteral("http://192.168.31.13:18131/api/v1");
        int requestTimeoutMs = 5000;
    };

    struct TrackingConfig {
        bool enabled = true;
        QString baseUrl = QStringLiteral("http://192.168.31.13:18130/api/v1");
        int requestTimeoutMs = 5000;
    };

    struct RoutePlanningConfig {
        double minEdgeCost = 1e-3;
        double edgePenalty = 0.01;
        double arcPenalty = 0.05;
    };


    struct VideoConfig {
        QString backend;
        QString streamUrl;
        QString controlBaseUrl;
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
        QString authToken;
        int statusPollIntervalMs = 100;
        int statusRequestTimeoutMs = 3000;
        int statusMaxBackoffMs = 5000;
        bool chassisAutoReconnect = true;
        int chassisReconnectIntervalMs = 1000;
        int chassisReconnectMaxIntervalMs = 15000;
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

    struct DatabaseConfig {
        QString backend = QStringLiteral("sqlite");
        QString connectionName = QStringLiteral("tenco_main");
        QString sqliteFilePath;
        QString host;
        int port = 5432;
        QString databaseName;
        QString userName;
        QString password;
        bool useWAL = true;
        bool foreignKeys = true;
        bool synchronousNormal = true;
        int busyTimeoutMs = 5000;
        int cacheSizePages = 2000;
        int connectTimeoutMs = 5000;
        int reconnectIntervalMs = 3000;
        bool enableTelemetryTables = true;
        bool enableLocalCache = true;
        bool enableAuditSync = true;
    };

    struct ConfigSnapshot {
        QJsonObject sourceDocument; // Preserve fields this version does not edit.
        QByteArray originalImportBytes; // Backup imported legacy fields before explicit migration.
        ManualControlConfig manualControl;
        PoseSourceConfig poseSource;
        TrackingConfig tracking;
        RoutePlanningConfig routePlanning;
        VideoConfig video;
        NetworkConfig network;
        GimbalConfig gimbal;
        LoggingConfig logging;
        DatabaseConfig database;
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
    bool createBackup(QString *errorMessage = nullptr, const QString &reason = QStringLiteral("manual"));
    bool restoreBackup(const QString &path, QString *errorMessage = nullptr, bool applyAfterRestore = true);
    QString diskRevision() const;
    QString expectedDiskRevision() const;
    QString runtimeRevision() const { return m_runtimeRevision; }

    const ManualControlConfig &manualControl() const { return m_manualControl; }
    const PoseSourceConfig &poseSource() const { return m_poseSource; }
    const TrackingConfig &tracking() const { return m_tracking; }
    const RoutePlanningConfig &routePlanning() const { return m_routePlanning; }
    const VideoConfig &video() const { return m_video; }
    const NetworkConfig &network() const { return m_network; }
    const GimbalConfig &gimbal() const { return m_gimbal; }
    const LoggingConfig &logging() const { return m_logging; }
    const DatabaseConfig &database() const { return m_database; }

    QString configFilePath() const { return m_configPath; }
    bool loadedFromFile() const { return m_loadedFromFile; }
    QStringList validationWarnings() const { return m_validationWarnings; }

signals:
    void configChanged();

private:
    explicit ConfigManager(QObject *parent = nullptr);
    void ensureLoaded();
    void load();
    void loadFromFile(const QString &path);
    void applyJsonObjectToCurrentConfig(const QJsonObject &root);
    QStringList validateJsonObject(const QJsonObject &root) const;
    void applyEnvironmentOverrides();
    bool saveToFile(const QString &path, QString *errorMessage = nullptr) const;
    bool backupBytes(const QByteArray &bytes, const QString &reason, QString *errorMessage) const;
    bool writeFileAtomically(const QByteArray &bytes, const QString &targetPath, QString *errorMessage = nullptr) const;
    void loadDefaults();
    void sanitizeConfig();
    QJsonObject toJsonObject() const;

    ManualControlConfig m_manualControl;
    PoseSourceConfig m_poseSource;
    TrackingConfig m_tracking;
    RoutePlanningConfig m_routePlanning;
    VideoConfig m_video;
    NetworkConfig m_network;
    GimbalConfig m_gimbal;
    LoggingConfig m_logging;
    DatabaseConfig m_database;
    QString m_configPath;
    QString m_configPathOverride;
    QStringList m_validationWarnings;
    bool m_loaded = false;
    bool m_loadedFromFile = false;
    bool m_startupBackupCreated = false;
    QJsonObject m_sourceDocument;
    QByteArray m_diskBytes;
    QString m_runtimeRevision;
    bool m_writeBlocked = false;
};

#endif // CONFIGMANAGER_H
