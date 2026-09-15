#pragma once
#include "chassisclient.h"
#include "motioncommandarbiter.h"
#include "poseclient.h"
#include "trackingclient.h"
#include <QPointer>

class ControlSessionCoordinator : public QObject {
    Q_OBJECT
  public:
    ControlSessionCoordinator(TrackingClient *tracking, PoseClient *pose, ChassisClient *chassis,
                              MotionCommandArbiter *manual, QObject *parent = nullptr);
    TrackingClient *tracking() const { return m_tracking; }
    PoseClient *pose() const { return m_pose; }
    void setBinding(const MapFrameBinding &binding);
    MapFrameBinding binding() const { return m_binding; }
    bool coordinatesReady() const;
    bool upload(const QJsonObject &plan);
    void acquireSession();
    void startTask();
    void resumeTask();
    void pauseTask();
    void abortTask();
    void requestManual();
    void emergencyStop();
    void resetFault();
    void shutdown();
  signals:
    void message(const QString &text);
    void bindingChanged(const MapFrameBinding &binding);
    void handoffChanged(bool pending);
    void manualInputsCleared();

  private:
    void beginMotion(const QString &operation);
    void checkHandoff();
    void cancelHandoff(const QString &reason = {});
    QPointer<TrackingClient> m_tracking;
    QPointer<PoseClient> m_pose;
    QPointer<ChassisClient> m_chassis;
    QPointer<MotionCommandArbiter> m_manual;
    MapFrameBinding m_binding;
    QString m_operation;
    QString m_expectedBoot, m_expectedTask, m_expectedExecution;
    quint64 m_expectedStateVersion = 0;
    QElapsedTimer m_handoffAge, m_stoppedAge;
    QTimer m_timer;
    QTimer m_resetTimer;
    QElapsedTimer m_resetAge;
};
