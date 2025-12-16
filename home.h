#ifndef HOME_H
#define HOME_H

#include <QObject>
#include <QJsonObject>
#include <QByteArray>
#include <QAbstractSocket>
#include <QNetworkReply>
#include <QProgressDialog>
#include <QQueue>
#include <QTimer>
#include <QList>
#include <QPointF>
#include <QtMath>

class QNetworkReply;
class QNetworkAccessManager;
class QWebSocket;
class QProgressDialog;
class QThread;
class HomeNetworkWorker;
class QImage;
class QFile;

namespace Ui {
class MainWindow;
}

class Home : public QObject
{
    Q_OBJECT

public:
    explicit Home(Ui::MainWindow *ui, QObject *parent = nullptr);
    ~Home() override;

    bool handleKeyPress(int key, bool isAutoRepeat);
    bool handleKeyRelease(int key, bool isAutoRepeat);

signals:
    void vehiclePoseUpdated(double x, double y, double theta);
    void routeSegmentCompleted(bool success);

public slots:
    void followRouteSegment(int fromPointId, int toPointId, const QList<QPointF> &polyline, double startTheta, double endTheta);
    void handleRouteQueueCompleted();
    void cancelRouteExecution();

private slots:
    void handleStatusPacket(const QJsonObject &packet);
    void handleNetworkFailure(int httpStatus, const QString &errorString, const QByteArray &responseBody);
    void checkConnectionRestored();

    void handleVideoTopicChanged(int index);
    void handleSavePathButtonClicked();
    void handleImageSwitchToggled(bool checked);
    void record();
    void photo();
    void restartControl();
    void orignsubmmit();
    void modesubmmit();
    void handleForwardButtonPressed();
    void handleForwardButtonReleased();
    void handleBackwardButtonPressed();
    void handleBackwardButtonReleased();
    void handleTurnLeftButtonPressed();
    void handleTurnLeftButtonReleased();
    void handleTurnRightButtonPressed();
    void handleTurnRightButtonReleased();
    void handleStopButtonClicked();
    void processRouteFollowerTick();
    void handleVideoReadyRead();
    void handleVideoStreamFinished();
    void handleVideoError(QNetworkReply::NetworkError error);
    void restartVideoStream();

private:
    static constexpr int kMotionRepeatIntervalMs = 40;
    static constexpr int kMaxVideoBufferSize = 3 * 1024 * 1024;

    struct RouteSegmentExecution {
        QList<QPointF> polyline;
        double startTheta = 0.0;
        double endTheta = 0.0;
        int targetIndex = 0;
    };

    Ui::MainWindow *ui;

    void initialize();
    void setupImageSwitches();
    void initializeMotionTimers();
    void initializeNetworkWorker();
    void initializeVideoDisplay();
    void connectWebSocket();

    bool isManualControlEnabledForButtons() const;
    bool isManualControlEnabledForKeys() const;

    void updateForwardTimer();
    void updateBackwardTimer();
    void updateTurnLeftTimer();
    void updateTurnRightTimer();

    bool shouldSendForward() const;
    bool shouldSendBackward() const;
    bool shouldSendTurnLeft() const;
    bool shouldSendTurnRight() const;

    void sendVelocityCommand(double xVel, double thetaVel);
    void sendForwardCommand();
    void sendBackwardCommand();
    void sendTurnLeftCommand();
    void sendTurnRightCommand();
    void startRecording();
    void stopRecordingAndSave();
    bool ensureSaveDirectorySelected(QWidget *parentForDialog);
    bool isVideoStreamActive() const;
    void logMessage(const QString &text);
    void sendRebootCommand();
    void updateRebootProgress();
    void sendStopLocation();
    void sendStartupWebSocketMessages();
    void startNextSegment();
    void finalizeCurrentSegment(bool success);
    double normalizeAngle(double angle) const;
    void sendRouteVelocity(double linear, double angular);
    double applySlewRate(double target, double lastValue, double accelLimit, double decelLimit, double dt) const;
    void resetRouteCommandState();
    void startVideoStream();
    void stopVideoStream();
    void scheduleVideoReconnect();
    void updateVideoPlaceholder(const QString &message);
    void displayVideoFrame(const QImage &image);
    bool isVideoDisplayReady() const;
    QString buildVideoUrlForTopic(const QString &topic) const;

    void onWebSocketConnected();
    void onWebSocketDisconnected();
    void onWebSocketError(QAbstractSocket::SocketError error);

    QTimer *restartCheckTimer;
    QWebSocket *webSocket;
    QTimer *forwardRepeatTimer;
    QTimer *backwardRepeatTimer;
    QTimer *turnLeftRepeatTimer;
    QTimer *turnRightRepeatTimer;
    QTimer *rebootCountdownTimer;
    QTimer *m_videoReconnectTimer = nullptr;

    QProgressDialog *rebootProgressDialog;

    RouteSegmentExecution m_activeRouteSegment;
    QQueue<RouteSegmentExecution> m_pendingRouteSegments;
    QTimer *m_routeFollowerTimer = nullptr;
    bool m_routeFollowerActive = false;
    bool m_poseValid = false;
    double m_currentPoseX = 0.0;
    double m_currentPoseY = 0.0;
    double m_currentPoseTheta = 0.0;
    double m_maxLinearSpeed = 0.5;
    double m_maxAngularSpeed = 0.5;
    double m_arrivalDistanceThreshold = 0.2;
    double m_arrivalAngleThresholdRad = 0.0;
    double m_linearGain = 0.8;
    double m_angularGain = 1.0;
    double m_headingStopThresholdRad = M_PI / 2.0;
    double m_headingSlowdownThresholdRad = M_PI / 4.0;
    double m_headingSlowdownFactor = 0.3;
    double m_nearTargetDistanceMultiplier = 3.0;
    double m_nearTargetSpeedMultiplier = 0.4;
    double m_linearAccelerationLimit = 0.6;
    double m_linearDecelerationLimit = 0.8;
    double m_angularAccelerationLimit = 1.2;
    double m_angularDecelerationLimit = 1.5;
    double m_finalAdjustLinearSpeed = 0.2;
    double m_finalAdjustAngularSpeed = 0.6;
    double m_lastRouteLinearCommand = 0.0;
    double m_lastRouteAngularCommand = 0.0;
    double m_routeFollowerDt = 0.1;
    double m_vehicleWheelBase = 0.6;
    double m_vehicleWheelDiameter = 0.2;
    double m_vehicleGearReduction = 1.0;

    HomeNetworkWorker *networkWorker;
    QThread *networkThread;
    QNetworkAccessManager *m_videoManager = nullptr;
    QNetworkReply *m_videoReply = nullptr;
    QByteArray m_videoBuffer;
    QByteArray m_recordBuffer;
    QString m_videoStreamTemplate;
    QString m_videoUrl;
    QString m_recordFilePath;
    QString m_saveDirectory;
    int m_videoReconnectIntervalMs = 2000;
    bool m_videoAutoStart = true;
    bool m_videoScaleContents = true;
    bool m_isRecording = false;
    QFile *m_recordFile = nullptr;
    QString m_activeVideoTopic;
    QImage m_lastVideoFrame;

    bool lastConnectionStatus;
    bool connectionRestored;

    bool forwardButtonHeld;
    bool forwardKeyHeld;
    bool backwardButtonHeld;
    bool backwardKeyHeld;
    bool turnLeftButtonHeld;
    bool turnLeftKeyHeld;
    bool turnRightButtonHeld;
    bool turnRightKeyHeld;
    bool startupMessagesSent;
    int rebootRemainingSeconds;
};

#endif // HOME_H
