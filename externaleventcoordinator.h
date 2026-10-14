#pragma once
#include "trackingclient.h"
#include <QTimer>

// Executes only stopped business actions. It never generates vehicle motion.
class ExternalEventCoordinator : public QObject {
    Q_OBJECT
  public:
    explicit ExternalEventCoordinator(TrackingClient *tracking, QObject *parent = nullptr);
    void configure(const QUrl &cameraUrl, const QString &token, const QString &journalDirectory,
                   int timeoutMs);
    bool resolveCurrent(const QString &expectedEventId, bool success);
  signals:
    void message(const QString &message);

  private:
    void tick();
    QString recordPath(const QString &id) const;
    QJsonObject readRecord(const QString &id) const;
    bool writeRecord(const QJsonObject &record);
    TrackingClient *m_tracking;
    JsonHttpClient m_camera;
    QTimer m_timer;
    QHash<QString, QJsonObject> m_events;
    QString m_directory, m_active, m_reported, m_ackSent;
};
