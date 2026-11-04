#ifndef HOME_STATUS_PRESENTER_H
#define HOME_STATUS_PRESENTER_H

#include "statusprotocol.h"
#include "trackingtypes.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <functional>


namespace Ui {
class MainWindow;
}
class QLineEdit;

class HomeStatusPresenter
{
public:
    using LogHandler = std::function<void(const QString &)>;

    HomeStatusPresenter(Ui::MainWindow *ui,
                        LogHandler logHandler);

    void handleStatusPacket(const QJsonObject &packet);
    bool handleNetworkFailure(int httpStatus, const QString &errorString, const QByteArray &responseBody);

    bool takeConnectionRestored();
    void clearConnectionRestored();
    static void showControlPose(QLineEdit *field, const ControlPoseSnapshot &pose);
    void setControlPoseStatus(const ControlPoseSnapshot &pose);
    void setPoseMonitoringAvailability(bool current, const QString &reason);
    void setTaskStatus(const TrackingSnapshot &status, bool current);

private:
    void setConnectionIndicator(bool connected);
    void applyStatusField(StatusProtocol::FieldId fieldId, const QJsonArray &values, const QJsonValue &firstValue);
    void renderLocalizationStatus();

    Ui::MainWindow *m_ui = nullptr;
    LogHandler m_logHandler;
    bool m_lastConnectionStatus = false;
    bool m_connectionRestored = false;
    ControlPoseSnapshot m_pose;
    bool m_hasPose = false;
    bool m_poseMonitorCurrent = false;
    QString m_poseMonitorReason;
};

#endif // HOME_STATUS_PRESENTER_H
