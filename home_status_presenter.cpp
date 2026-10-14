#include "home_status_presenter.h"

#include "ui_mainwindow.h"

#include <QJsonValue>
#include <QMap>
#include <QObject>
#include <QStringList>
#include <QtMath>

#include <utility>

namespace {
constexpr const char *kConnectedStyle =
    "background-color: #55ff00; min-width: 80px; min-height: 50px; border: 1px solid white; "
    "border-radius: 10px; font-size: 18px; font-family: 微软雅黑;";
constexpr const char *kDisconnectedStyle =
    "background-color: #ff0000; min-width: 80px; min-height: 50px; border: 1px solid white; "
    "border-radius: 10px; font-size: 18px; font-family: 微软雅黑;";
}

HomeStatusPresenter::HomeStatusPresenter(Ui::MainWindow *ui,
                                         LogHandler logHandler)
    : m_ui(ui)
    , m_logHandler(std::move(logHandler))
{
}

void HomeStatusPresenter::handleStatusPacket(const QJsonObject &packet)
{
    const bool wasConnected = m_lastConnectionStatus;
    m_lastConnectionStatus = true;
    if (!wasConnected) {
        m_connectionRestored = true;
        if (m_logHandler) {
            m_logHandler(QObject::tr("状态通信已恢复"));
        }
    }

    setConnectionIndicator(true);

    const QJsonArray dataArray = packet.value(QStringLiteral("data")).toArray();
    for (const QJsonValue &item : dataArray) {
        const QJsonObject data = item.toObject();
        const QString addr = data.value(QStringLiteral("address")).toString();
        const QJsonArray values = data.value(QStringLiteral("value")).toArray();
        const QJsonValue firstValue = values.isEmpty() ? QJsonValue() : values.first();
        applyStatusField(StatusProtocol::fieldIdFromAddress(addr), values, firstValue);
    }
}

bool HomeStatusPresenter::handleNetworkFailure(int httpStatus, const QString &errorString, const QByteArray &responseBody)
{
    const bool wasConnected = m_lastConnectionStatus;
    m_lastConnectionStatus = false;
    m_connectionRestored = false;
    setConnectionIndicator(false);

    QString detail = QObject::tr("通信故障，正在尝试重连");
    if (httpStatus > 0) {
        detail += QObject::tr(" (HTTP %1)").arg(httpStatus);
    }
    if (!errorString.isEmpty()) {
        detail += QObject::tr("：%1").arg(errorString);
    }
    if (m_logHandler) {
        m_logHandler(detail);
    }

    if (!responseBody.isEmpty() && m_logHandler) {
        const QString bodyPreview = QString::fromUtf8(responseBody.left(200)).trimmed();
        if (!bodyPreview.isEmpty()) {
            m_logHandler(QObject::tr("响应: %1").arg(bodyPreview));
        }
    }


    return wasConnected;
}

bool HomeStatusPresenter::takeConnectionRestored()
{
    if (!m_connectionRestored) {
        return false;
    }
    m_connectionRestored = false;
    return true;
}

void HomeStatusPresenter::clearConnectionRestored()
{
    m_connectionRestored = false;
}

void HomeStatusPresenter::setConnectionIndicator(bool connected)
{
    if (!m_ui || !m_ui->pushButton_10) {
        return;
    }
    if (connected) {
        m_ui->pushButton_10->setText(QObject::tr("通信正常"));
        m_ui->pushButton_10->setStyleSheet(QString::fromUtf8(kConnectedStyle));
    } else {
        m_ui->pushButton_10->setText(QObject::tr("通信故障"));
        m_ui->pushButton_10->setStyleSheet(QString::fromUtf8(kDisconnectedStyle));
    }
}

void HomeStatusPresenter::applyStatusField(StatusProtocol::FieldId fieldId,
                                           const QJsonArray &values,
                                           const QJsonValue &firstValue)
{
    if (!m_ui) {
        return;
    }

    switch (fieldId) {
    case StatusProtocol::FieldId::BatteryPercent: {
        const int battery = firstValue.toInt();
        if (m_ui->battery) {
            m_ui->battery->setValue(battery);
        }
        if (m_ui->lable_battray) {
            m_ui->lable_battray->setText(QString::number(battery) + "%");
        }
        break;
    }
    case StatusProtocol::FieldId::BatteryVoltage:
        if (m_ui->lineEdit_BattryVol) {
            m_ui->lineEdit_BattryVol->setText(QString::number(firstValue.toDouble(), 'f', 2) + "V");
        }
        break;
    case StatusProtocol::FieldId::WorkMode: {
        static const QMap<int, QString> modeMap{
            {0, QStringLiteral("维护模式")},
            {1, QStringLiteral("手动模式")},
            {2, QStringLiteral("自动模式")}
        };
        if (m_ui->lineEdit_mode) {
            m_ui->lineEdit_mode->setText(QStringLiteral("当前模式:   ")
                                         + modeMap.value(firstValue.toInt(), QStringLiteral("未知模式")));
        }
        break;
    }
    case StatusProtocol::FieldId::MapName:
        if (m_ui->lineEdit_mapname) {
            m_ui->lineEdit_mapname->setText(QStringLiteral("当前地图:   ") + firstValue.toString());
        }
        break;
    case StatusProtocol::FieldId::Velocity:
        if (m_ui->lcdNumber && !values.isEmpty()) {
            const double velocity = values.first().toDouble();
            if (velocity >= -2.0 && velocity <= 2.0) {
                m_ui->lcdNumber->setDigitCount(5);
                m_ui->lcdNumber->display(QString::number(qAbs(velocity), 'f', 2));
            }
        }
        break;
    case StatusProtocol::FieldId::BatteryTemperature:
        if (m_ui->lineEdit_BattryTemp) {
            m_ui->lineEdit_BattryTemp->setText(QString::number(firstValue.toInt()) + "℃");
        }
        break;
    case StatusProtocol::FieldId::ChargeDischargeState: {
        QString batteryState = QStringLiteral("电已充满");
        if (firstValue.toInt() == 1) {
            batteryState = QStringLiteral("充电中");
        } else if (firstValue.toInt() == 2) {
            batteryState = QStringLiteral("放电中");
        }
        if (m_ui->lineEdit_BattryState) {
            m_ui->lineEdit_BattryState->setText(batteryState);
        }
        break;
    }
    case StatusProtocol::FieldId::RunTime: {
        const int hour = values.size() > 0 ? values.at(0).toInt() : 0;
        const int minute = values.size() > 1 ? values.at(1).toInt() : 0;
        const int second = values.size() > 2 ? values.at(2).toInt() : 0;
        if (m_ui->lineEdit_RunTime) {
            m_ui->lineEdit_RunTime->setText(QStringLiteral("%1小时%2分钟%3秒").arg(hour).arg(minute).arg(second));
        }
        break;
    }
    case StatusProtocol::FieldId::Unknown:
        break;
    }
}
