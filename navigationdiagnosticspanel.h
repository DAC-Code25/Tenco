#pragma once

#include "jsonhttpclient.h"
#include <QElapsedTimer>
#include <QHash>
#include <QPointer>
#include <QTimer>
#include <QWidget>

class QLabel;
class QFrame;
class QGridLayout;
class QProgressBar;
class QResizeEvent;
class TrackingClient;

struct NavigationDiagnosticsConnection {
    QUrl poseUrl;
    QUrl trackingUrl;
    QString token;
    int poseTimeoutMs = 3000;
    int trackingTimeoutMs = 3000;
    bool poseEnabled = true;
    bool trackingEnabled = true;
    QUrl managementUrl;
};

// Read-only observation; this panel never acquires a session or sends a control command.
class NavigationDiagnosticsPanel : public QWidget {
    Q_OBJECT
  public:
    explicit NavigationDiagnosticsPanel(QWidget *parent = nullptr);
    ~NavigationDiagnosticsPanel() override;
    void configure(const NavigationDiagnosticsConnection &connection);
    void setTrackingClient(TrackingClient *client);

  protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

  private:
    void poll();
    void clearDiagnostics(const QString &message);
    void clearTracking(const QString &message);
    void clearResources(const QString &message);
    void renderDiagnostics(const QJsonObject &object);
    void renderResources(const QJsonObject &object);
    void renderTracking();
    void readSharedTrackingStatus();
    bool readSharedTrackingHealth();
    void setValue(const QString &key, const QString &text, const QString &tone = {});
    void setDetail(const QString &key, const QString &text);
    void setResource(const QString &key, const QJsonValue &percent, const QString &detail = {});
    QFrame *createStatusCard(const QString &key, const QString &title, bool sensor);
    void arrangeCards();

    JsonHttpClient m_poseHttp;
    JsonHttpClient m_trackingHttp;
    JsonHttpClient m_resourcesHttp;
    QTimer m_timer;
    QElapsedTimer m_diagnosticsAge;
    QElapsedTimer m_sampleProgressAge;
    double m_lastSampledAt = 0;
    QElapsedTimer m_resourcesAge;
    QElapsedTimer m_resourcesProgressAge;
    double m_lastResourcesSampledAt = 0;
    double m_resourcesReceivedAgeMs = 0;
    QLabel *m_resourcesState = nullptr;
    QElapsedTimer m_healthAge;
    qint64 m_healthReceivedAgeMs = 0;
    QElapsedTimer m_statusAge;
    QJsonObject m_health;
    QJsonObject m_status;
    QPointer<TrackingClient> m_trackingClient;
    QHash<QString, QLabel *> m_values;
    QHash<QString, QFrame *> m_lights;
    QHash<QString, QFrame *> m_badges;
    QHash<QString, QLabel *> m_details;
    QHash<QString, QLabel *> m_notes;
    QHash<QString, QProgressBar *> m_bars;
    QGridLayout *m_summaryGrid = nullptr;
    QGridLayout *m_bodyGrid = nullptr;
    QGridLayout *m_sensorGrid = nullptr;
    QList<QWidget *> m_summaryCards;
    QList<QWidget *> m_sensorCards;
    QWidget *m_sensorSection = nullptr;
    QWidget *m_resourceSection = nullptr;
    bool m_compact = false;
    NavigationDiagnosticsConnection m_connection;
};
