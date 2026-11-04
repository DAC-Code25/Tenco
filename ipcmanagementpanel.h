#pragma once

#include "jsonhttpclient.h"
#include <QElapsedTimer>
#include <QHash>
#include <QPointer>
#include <QTimer>
#include <QWidget>
#include <functional>

class QComboBox;
class QDialog;
class QFormLayout;
class QLabel;
class QPushButton;
class QTabWidget;

struct IpcManagementConnection {
    QUrl baseUrl;
    QString token;
    int timeoutMs = 3000;
};

// A separate, low-rate maintenance channel. It never obtains a task session or
// sends motion commands, and never writes configuration merely by opening UI.
class IpcManagementPanel : public QWidget {
    Q_OBJECT
public:
    explicit IpcManagementPanel(QWidget *parent = nullptr);
    ~IpcManagementPanel() override;
    void setConnectionProvider(std::function<IpcManagementConnection()> provider);
    void refreshOriginSnapshot();
    bool hasUnsavedChanges() const;
    void showSessionReleaseResult(bool released, const QString &message);
    void addMaintenancePage(const QString &title, QWidget *content);

signals:
    void releaseTaskSessionRequested();
    void originSnapshotChanged(const QJsonObject &origin);

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    void refresh();
    void refreshOriginOnly();
    bool configureConnection();
    void renderSnapshot(const QJsonObject &snapshot);
    void readParameters();
    void loadFields(const QJsonObject &component);
    void updateParameterReadbacks(const QJsonObject &component);
    void writeParameters(bool apply, bool savedOnly = false);
    void operateService(const QString &id, const QString &action);
    void confirmFusionOrigin(std::function<void(QJsonObject)> confirmed);
    QString serviceCondition(const QString &id, const QString &action) const;
    QJsonObject serviceSnapshot(const QString &id) const;
    void beginOperation(const QString &path, QJsonObject body);
    void pollOperation();
    void refreshBackups();
    void updateButtons();
    void setResult(const QString &text, bool error = false);
    QJsonObject currentValues() const;
    QJsonObject componentSnapshot() const;

    JsonHttpClient m_http;
    JsonHttpClient m_snapshotHttp;
    QTimer m_timer;
    std::function<IpcManagementConnection()> m_provider;
    IpcManagementConnection m_connection;
    QJsonObject m_snapshot;
    QJsonObject m_originalValues;
    QHash<QString, QWidget *> m_fields;
    QHash<QString, QLabel *> m_parameterReadbacks;
    QHash<QString, QLabel *> m_serviceStates;
    QList<QPushButton *> m_serviceButtons;
    QComboBox *m_component = nullptr;
    QTabWidget *m_pages = nullptr;
    QFormLayout *m_form = nullptr;
    QLabel *m_connectionState = nullptr;
    QLabel *m_originState = nullptr;
    QLabel *m_serviceHint = nullptr;
    QLabel *m_parameterState = nullptr;
    QLabel *m_parameterEditState = nullptr;
    QLabel *m_parameterTarget = nullptr;
    QLabel *m_backupState = nullptr;
    QPushButton *m_backupCreate = nullptr;
    QPushButton *m_backupRefresh = nullptr;
    QLabel *m_result = nullptr;
    QPushButton *m_read = nullptr;
    QPushButton *m_save = nullptr;
    QPushButton *m_saveApply = nullptr;
    QPushButton *m_applySaved = nullptr;
    QString m_loadedComponent;
    QString m_loadedRevision;
    QString m_operationId;
    QString m_bootId;
    QElapsedTimer m_operationAge;
    QPointer<QDialog> m_originDialog;
    bool m_online = false;
    bool m_loading = false;
    bool m_stale = false;
    bool m_readAfterRefresh = false;
    bool m_parameterOperation = false;
    bool m_operationRefreshPending = false;
    bool m_backupsLoaded = false;
    bool m_legacyOriginEndpoint = false;
    quint64 m_backupGeneration = 0;
    int m_backoffMs = 2000;
};
