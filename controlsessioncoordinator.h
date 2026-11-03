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
    bool actionPending() const { return !m_action.isEmpty(); }
    bool upload(const QJsonObject &plan);
    bool uploadAndStart(const QJsonObject &plan);
    void cancelPendingAction();
    bool confirmReadTask(const QJsonObject &record);
    bool updateOrigin(const QJsonObject &origin, const QString &expectedRevision);
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
    void actionPendingChanged(bool pending);

  private:
    void beginMotion(const QString &operation);
    bool queueAction(const QString &operation, const QJsonObject &payload = {});
    void advanceAction();
    void cancelAction(const QString &reason = {});
    void requestStop(const QString &operation);
    void observeStop(const TrackingSnapshot &status);
    QPointer<TrackingClient> m_tracking;
    QPointer<PoseClient> m_pose;
    MapFrameBinding m_binding;
    QTimer m_actionTimer;
    QElapsedTimer m_actionAge;
    QString m_action, m_actionBoot, m_actionTask, m_actionExecution, m_actionState;
    QJsonObject m_actionPayload;
    QString m_prepareRequestId;
    int m_actionRevision = 0;
    quint64 m_actionVersion = 0, m_readyVersion = 0;
    bool m_prepareApplied = false, m_readyObserved = false;
    int m_sessionAttempts = 0;
    qint64 m_nextSessionAttempt = 0;
    QString m_stopRequest, m_stopOperation, m_stopBoot, m_stopExecution;
    quint64 m_stopSeq = 0;
    bool m_stopApplied = false;
};
