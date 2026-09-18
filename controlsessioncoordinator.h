#pragma once
#include "poseclient.h"
#include "trackingclient.h"
#include <QPointer>

class ControlSessionCoordinator : public QObject {
    Q_OBJECT
  public:
    ControlSessionCoordinator(TrackingClient *tracking, PoseClient *pose, QObject *parent = nullptr);
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
    void emergencyStop();
    void resetFault();
    void shutdown();
  signals:
    void message(const QString &text);
    void bindingChanged(const MapFrameBinding &binding);

  private:
    void beginMotion(const QString &operation);
    QPointer<TrackingClient> m_tracking;
    QPointer<PoseClient> m_pose;
    MapFrameBinding m_binding;
};
