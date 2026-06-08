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

#include "gimbalcontrolclient.h"

class QProgressDialog;
class AbstractVideoSource;
class CameraControlClient;
class StatusClient;
class ChassisClient;
class RouteFollower;
class MotionCommandArbiter;
class HomeStatusPresenter;
class HomeVideoPresenter;

namespace Ui {
class MainWindow;
}

class Home : public QObject
{
    Q_OBJECT

public:
    explicit Home(Ui::MainWindow *ui, QObject *parent = nullptr);
    ~Home() override;

    bool handleKeyPress(int key, Qt::KeyboardModifiers modifiers, bool isAutoRepeat);
    bool handleKeyRelease(int key, Qt::KeyboardModifiers modifiers, bool isAutoRepeat);

signals:
    void vehiclePoseUpdated(double x, double y, double theta);
    void routeSegmentCompleted(bool success);

public slots:
    void followRouteSegment(int fromPointId, int toPointId, const QList<QPointF> &polyline, double startTheta, double endTheta);
    void handleRouteQueueCompleted();
    void cancelRouteExecution();
    void stopMotionForSafety(const QString &reason);

private slots:
    void applyRuntimeConfig();
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
    void handleGimbalUpPressed();
    void handleGimbalDownPressed();
    void handleGimbalLeftPressed();
    void handleGimbalRightPressed();
    void handleGimbalPitchUpPressed();
    void handleGimbalPitchDownPressed();
    void handleGimbalButtonReleased();

    void handleChassisConnected();
    void handleChassisDisconnected();
    void handleChassisError(const QString &errorString);
    void handleVideoFrameReceived(const QImage &frame);

private:
    Ui::MainWindow *ui;

    void initialize();
    void setupImageSwitches();
    void initializeMotionControl();
    void applyRouteFollowerConfig();
    void applyStatusClientConfig();
    void applyChassisClientConfig();
    void applyVideoConfig();
    void applyCameraControlConfig();
    void applyGimbalControlConfig();
    void initializeVideoDisplay();
    void initializeCameraStatusPolling();
    void populateVideoStreamSelector();
    bool applyConfiguredVideoStreamSelection();

    bool isManualControlEnabledForButtons() const;
    bool isManualControlEnabledForKeys() const;
    void updateManualCommandConfig();

    bool shouldSendForward() const;
    bool shouldSendBackward() const;
    bool shouldSendTurnLeft() const;
    bool shouldSendTurnRight() const;

    void sendVelocityCommand(double xVel, double thetaVel);
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
    void initializeGimbalControl();
    void updateGimbalStatus(const GimbalStatus &status);
    void jogGimbal(GimbalControlClient::Axis axis, GimbalControlClient::Direction direction, const QString &actionText);
    void stopGimbal();
    void stopGimbalKeyboardMotion();
    bool canJogGimbal(GimbalControlClient::Axis axis, GimbalControlClient::Direction direction, QString *reason) const;
    void updateGimbalButtonState();
    bool handleGimbalKeyPress(int key, Qt::KeyboardModifiers modifiers);
    bool handleGimbalKeyRelease(int key, Qt::KeyboardModifiers modifiers);
    void updateGimbalKeyboardMotion();

    QTimer *restartCheckTimer;
    QTimer *rebootCountdownTimer;
    QTimer *cameraStatusTimer;
    QTimer *gimbalSafetyStopTimer;

    QProgressDialog *rebootProgressDialog;

    StatusClient *m_statusClient = nullptr;
    ChassisClient *m_chassisClient = nullptr;
    AbstractVideoSource *m_videoSource = nullptr;
    CameraControlClient *m_cameraControlClient = nullptr;
    GimbalControlClient *m_gimbalControlClient = nullptr;
    RouteFollower *m_routeFollower = nullptr;
    MotionCommandArbiter *m_motionArbiter = nullptr;
    std::unique_ptr<HomeStatusPresenter> m_statusPresenter;
    std::unique_ptr<HomeVideoPresenter> m_videoPresenter;

    QString m_recordFilePath;
    QString m_saveDirectory;
    QString m_activeVideoTopic;
    bool m_remoteCameraServiceAvailable = false;
    bool m_remoteCameraConnected = false;
    bool m_remoteCameraRecording = false;
    QString m_lastRemoteCameraStatusMessage;
    bool m_lastLoggedRemoteCameraConnected = false;
    bool m_lastLoggedRemoteCameraRecording = false;
    bool m_hasLoggedRemoteCameraStatus = false;
    QHash<QString, QString> m_videoStreamOptions;
    QString m_lastHomeLogMessage;
    GimbalStatus m_lastGimbalStatus;
    bool m_hasGimbalStatus = false;
    bool m_gimbalMoving = false;
    GimbalControlClient::Axis m_activeGimbalAxis = GimbalControlClient::Axis::Height;
    GimbalControlClient::Direction m_activeGimbalDirection = GimbalControlClient::Direction::Value1;
    QString m_activeGimbalAction;

    bool forwardButtonHeld;
    bool forwardKeyHeld;
    bool backwardButtonHeld;
    bool backwardKeyHeld;
    bool turnLeftButtonHeld;
    bool turnLeftKeyHeld;
    bool turnRightButtonHeld;
    bool turnRightKeyHeld;
    bool gimbalKeyUpHeld = false;
    bool gimbalKeyDownHeld = false;
    bool gimbalKeyLeftHeld = false;
    bool gimbalKeyRightHeld = false;
    bool gimbalLeftCtrlHeld = false;
    bool gimbalKeyboardMotionActive = false;
    GimbalControlClient::Axis m_activeGimbalKeyboardAxis = GimbalControlClient::Axis::Height;
    GimbalControlClient::Direction m_activeGimbalKeyboardDirection = GimbalControlClient::Direction::Value1;
    int m_manualMotionRepeatIntervalMs = 40;
    int rebootRemainingSeconds;
};

#endif // HOME_H
