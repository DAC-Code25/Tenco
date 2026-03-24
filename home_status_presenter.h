#ifndef HOME_STATUS_PRESENTER_H
#define HOME_STATUS_PRESENTER_H

#include "statusprotocol.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <functional>

class RouteFollower;

namespace Ui {
class MainWindow;
}

class HomeStatusPresenter
{
public:
    using LogHandler = std::function<void(const QString &)>;
    using PoseHandler = std::function<void(double x, double y, double theta)>;

    HomeStatusPresenter(Ui::MainWindow *ui,
                        RouteFollower *routeFollower,
                        LogHandler logHandler,
                        PoseHandler poseHandler);

    void handleStatusPacket(const QJsonObject &packet);
    bool handleNetworkFailure(int httpStatus, const QString &errorString, const QByteArray &responseBody);

    bool takeConnectionRestored();
    void clearConnectionRestored();

private:
    void setConnectionIndicator(bool connected);
    void applyStatusField(StatusProtocol::FieldId fieldId, const QJsonArray &values, const QJsonValue &firstValue);

    Ui::MainWindow *m_ui = nullptr;
    RouteFollower *m_routeFollower = nullptr;
    LogHandler m_logHandler;
    PoseHandler m_poseHandler;
    bool m_lastConnectionStatus = false;
    bool m_connectionRestored = false;
};

#endif // HOME_STATUS_PRESENTER_H
