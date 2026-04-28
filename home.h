#ifndef HOME_H
#define HOME_H

#include <QObject>
#include <QJsonObject>
#include <QProgressDialog>
#include <QTimer>
#include <QList>
#include <QHash>
#include <QPointF>
#include <QImage>

#include <memory>

class QProgressDialog;
class AbstractVideoSource;
class CameraControlClient;
class StatusClient;
class ChassisClient;
class RouteFollower;
class HomeStatusPresenter;

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
    void submitOriginCommand();
    void submitModeCommand();
    void handleForwardButtonPressed();
    void handleForwardButtonReleased();
    void handleBackwardButtonPressed();
    void handleBackwardButtonReleased();
    void handleTurnLeftButtonPressed();
    void handleTurnLeftButtonReleased();
    void handleTurnRightButtonPressed();
    void handleTurnRightButtonReleased();
    void handleStopButtonClicked();

    void handleChassisConnected();
    void handleChassisDisconnected();
    void handleChassisError(const QString &errorString);
    void handleVideoFrameReceived(const QImage &frame);

private:
    static constexpr int kMotionRepeatIntervalMs = 40;

    Ui::MainWindow *ui;

    void initialize();
    void setupImageSwitches();
    void initializeMotionTimers();
    void initializeVideoDisplay();
    void initializeCameraStatusPolling();
    void populateVideoStreamSelector();
    bool applyConfiguredVideoStreamSelection();

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
    void logMessage(const QString &text);
    void updateRebootProgress();
    void updateVideoPlaceholder(const QString &message);
    void displayVideoFrame(const QImage &image);
    bool isVideoDisplayReady() const;
    bool usesRemoteCameraControl() const;
    void initializeCameraControl();
    void updateRecordButtonText(bool remoteRecordingActive);
    void updateRemoteCameraUiState();
    void requestRemoteCameraStatus();
    void logCameraStatusChange(const QString &message);

    QTimer *restartCheckTimer;
    QTimer *forwardRepeatTimer;
    QTimer *backwardRepeatTimer;
    QTimer *turnLeftRepeatTimer;
    QTimer *turnRightRepeatTimer;
    QTimer *rebootCountdownTimer;
    QTimer *cameraStatusTimer;

    QProgressDialog *rebootProgressDialog;

    StatusClient *m_statusClient = nullptr;
    ChassisClient *m_chassisClient = nullptr;
    AbstractVideoSource *m_videoSource = nullptr;
    CameraControlClient *m_cameraControlClient = nullptr;
    RouteFollower *m_routeFollower = nullptr;
    std::unique_ptr<HomeStatusPresenter> m_statusPresenter;

    QString m_recordFilePath;
    QString m_saveDirectory;
    bool m_videoScaleContents = true;
    QString m_activeVideoTopic;
    QImage m_lastVideoFrame;
    bool m_remoteCameraServiceAvailable = false;
    bool m_remoteCameraConnected = false;
    bool m_remoteCameraRecording = false;
    QString m_lastRemoteCameraStatusMessage;
    bool m_lastLoggedRemoteCameraConnected = false;
    bool m_lastLoggedRemoteCameraRecording = false;
    bool m_hasLoggedRemoteCameraStatus = false;
    QHash<QString, QString> m_videoStreamOptions;
    QString m_lastHomeLogMessage;

    bool forwardButtonHeld;
    bool forwardKeyHeld;
    bool backwardButtonHeld;
    bool backwardKeyHeld;
    bool turnLeftButtonHeld;
    bool turnLeftKeyHeld;
    bool turnRightButtonHeld;
    bool turnRightKeyHeld;
    int rebootRemainingSeconds;
};

#endif // HOME_H
