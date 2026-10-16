#pragma once
#include "camerasettingsclient.h"
#include <QWidget>
#include <functional>
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
struct CameraSettingsConnection { QUrl url; QString token; int timeoutMs = 5000; };

class CameraSettingsPanel : public QWidget {
    Q_OBJECT
public:
    explicit CameraSettingsPanel(QWidget *parent = nullptr);
    void setConnectionProvider(std::function<CameraSettingsConnection()> provider) { m_provider = std::move(provider); }
private:
    bool prepareConnection();
    void load(const QJsonObject &config, const QJsonObject &capabilities);
    void updateEnabled();
    QJsonObject editedSettings() const;
    CameraSettingsClient m_client;
    std::function<CameraSettingsConnection()> m_provider;
    CameraSettingsConnection m_connection;
    QJsonObject m_config;
    QComboBox *m_size[3];
    QDoubleSpinBox *m_fps[3], *m_sensorFps;
    QLabel *m_native, *m_result, *m_runtime;
    QPushButton *m_read, *m_apply;
    bool m_ready = false;
};
