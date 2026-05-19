#include "gimbalcontrolclient.h"

#include <QAbstractSocket>
#include <QDataStream>
#include <QLoggingCategory>
#include <QTcpSocket>
#include <QTimer>
#include <QtGlobal>

Q_LOGGING_CATEGORY(lcGimbalControlClient, "tenco.gimbal.control")

namespace {
constexpr quint16 kModbusProtocolId = 0;
constexpr quint8 kFunctionReadHolding = 0x03;
constexpr quint8 kFunctionWriteSingle = 0x06;
constexpr quint8 kFunctionWriteMultiple = 0x10;
constexpr int kMinimumStatusRegisterCount = 7;

quint8 byteAt(const QByteArray &data, int index)
{
    return static_cast<quint8>(data.at(index));
}

quint16 readU16(const QByteArray &data, int index)
{
    return static_cast<quint16>((byteAt(data, index) << 8) | byteAt(data, index + 1));
}

void appendU16(QByteArray &data, quint16 value)
{
    data.append(static_cast<char>((value >> 8) & 0xff));
    data.append(static_cast<char>(value & 0xff));
}
}

GimbalControlClient::GimbalControlClient(QObject *parent)
    : QObject(parent)
    , m_socket(new QTcpSocket(this))
    , m_requestTimeoutTimer(new QTimer(this))
    , m_statusPollTimer(new QTimer(this))
{
    m_requestTimeoutTimer->setSingleShot(true);
    connect(m_requestTimeoutTimer, &QTimer::timeout, this, &GimbalControlClient::handleRequestTimeout);

    m_statusPollTimer->setSingleShot(false);
    connect(m_statusPollTimer, &QTimer::timeout, this, &GimbalControlClient::handleStatusPollTimeout);

    connect(m_socket, &QTcpSocket::connected, this, &GimbalControlClient::handleSocketConnected);
    connect(m_socket, &QTcpSocket::disconnected, this, &GimbalControlClient::handleSocketDisconnected);
    connect(m_socket, &QTcpSocket::readyRead, this, &GimbalControlClient::handleSocketReadyRead);
    connect(m_socket, &QTcpSocket::errorOccurred, this, &GimbalControlClient::handleSocketError);
}

GimbalControlClient::~GimbalControlClient()
{
    stopStatusPolling();
    if (m_socket) {
        m_socket->abort();
    }
}

void GimbalControlClient::configure(const Settings &settings)
{
    const bool hostChanged = m_settings.host != settings.host ||
                             m_settings.port != settings.port ||
                             m_settings.unitId != settings.unitId;
    m_settings = settings;
    m_settings.host = m_settings.host.trimmed();
    m_settings.port = qMax(1, m_settings.port);
    m_settings.unitId = qBound(1, m_settings.unitId, 255);
    m_settings.requestTimeoutMs = qBound(200, m_settings.requestTimeoutMs, 10000);
    m_settings.statusPollIntervalMs = qBound(100, m_settings.statusPollIntervalMs, 10000);
    m_settings.statusRegisterCount = qMax(kMinimumStatusRegisterCount, m_settings.statusRegisterCount);

    m_statusPollTimer->setInterval(m_settings.statusPollIntervalMs);
    m_requestTimeoutTimer->setInterval(m_settings.requestTimeoutMs);

    if (hostChanged && m_socket->state() != QAbstractSocket::UnconnectedState) {
        m_socket->abort();
    }
}

bool GimbalControlClient::isConfigured() const
{
    return m_settings.enabled && !m_settings.host.isEmpty() && m_settings.port > 0;
}

void GimbalControlClient::startStatusPolling()
{
    if (!isConfigured()) {
        return;
    }
    if (!m_statusPollTimer->isActive()) {
        m_statusPollTimer->start();
    }
    requestStatus();
}

void GimbalControlClient::stopStatusPolling()
{
    m_statusPollTimer->stop();
}

void GimbalControlClient::requestStatus()
{
    if (!isConfigured()) {
        return;
    }
    if ((m_hasPendingRequest && m_pendingRequest.type == RequestType::ReadStatus) || hasQueuedStatusRequest()) {
        return;
    }

    Request request;
    request.type = RequestType::ReadStatus;
    request.address = static_cast<quint16>(m_settings.statusStartAddress);
    request.values = {static_cast<quint16>(m_settings.statusRegisterCount)};
    request.operation = QStringLiteral("gimbal_status");
    enqueueRequest(request);
}

void GimbalControlClient::jog(Axis axis, Direction direction)
{
    Request request;
    request.type = RequestType::WriteSingle;
    request.address = controlAddressForAxis(axis);
    request.values = {static_cast<quint16>(direction)};
    request.operation = QStringLiteral("gimbal_%1_%2").arg(axisName(axis), directionName(direction));
    enqueueRequest(request, true);
}

void GimbalControlClient::stopAxis(Axis axis)
{
    Request request;
    request.type = RequestType::WriteSingle;
    request.address = controlAddressForAxis(axis);
    request.values = {0};
    request.operation = QStringLiteral("gimbal_%1_stop").arg(axisName(axis));
    enqueueRequest(request, true);
}

void GimbalControlClient::stopAll()
{
    Request request;
    request.type = RequestType::WriteMultiple;
    request.address = static_cast<quint16>(m_settings.heightControlAddress);
    request.values = {0, 0, 0};
    request.operation = QStringLiteral("gimbal_stop_all");
    enqueueRequest(request, true);
}

QString GimbalControlClient::axisName(Axis axis)
{
    switch (axis) {
    case Axis::Height:
        return QStringLiteral("height");
    case Axis::Pitch:
        return QStringLiteral("pitch");
    case Axis::Yaw:
        return QStringLiteral("yaw");
    }
    return QStringLiteral("unknown");
}

QString GimbalControlClient::directionName(Direction direction)
{
    switch (direction) {
    case Direction::Value1:
        return QStringLiteral("value1");
    case Direction::Value2:
        return QStringLiteral("value2");
    }
    return QStringLiteral("unknown");
}

void GimbalControlClient::handleSocketConnected()
{
    emit connectionStateChanged(true);
    if (m_hasPendingRequest && m_pendingRequest.transactionId == 0) {
        sendPendingRequest();
    }
}

void GimbalControlClient::handleSocketDisconnected()
{
    emit connectionStateChanged(false);
}

void GimbalControlClient::handleSocketError()
{
    if (!m_hasPendingRequest) {
        return;
    }
    failPendingRequest(m_socket->errorString());
}

void GimbalControlClient::handleSocketReadyRead()
{
    m_receiveBuffer.append(m_socket->readAll());
    if (!m_hasPendingRequest || m_receiveBuffer.size() < 6) {
        return;
    }

    const quint16 length = readU16(m_receiveBuffer, 4);
    const int responseSize = 6 + length;
    if (m_receiveBuffer.size() < responseSize) {
        return;
    }

    const QByteArray response = m_receiveBuffer.left(responseSize);
    m_receiveBuffer.remove(0, responseSize);
    parsePendingResponse(response);
}

void GimbalControlClient::handleRequestTimeout()
{
    if (!m_hasPendingRequest) {
        return;
    }
    const QString message = m_pendingRequest.transactionId == 0 ? tr("连接 PLC 超时") : tr("PLC 响应超时");
    if (m_socket->state() != QAbstractSocket::UnconnectedState) {
        m_socket->abort();
    }
    failPendingRequest(message);
}

void GimbalControlClient::handleStatusPollTimeout()
{
    requestStatus();
}

void GimbalControlClient::enqueueRequest(const Request &request, bool urgent)
{
    if (!isConfigured()) {
        emit commandFailed(request.operation, tr("云台 PLC 未配置或未启用"));
        return;
    }

    if (urgent) {
        m_requestQueue.clear();
        if (m_hasPendingRequest) {
            if (m_socket->state() != QAbstractSocket::UnconnectedState) {
                m_socket->abort();
            }
            m_requestTimeoutTimer->stop();
            m_hasPendingRequest = false;
            m_pendingRequest = Request{};
            m_receiveBuffer.clear();
        }
    }

    m_requestQueue.enqueue(request);
    processNextRequest();
}

void GimbalControlClient::processNextRequest()
{
    if (m_hasPendingRequest || m_requestQueue.isEmpty()) {
        return;
    }

    m_pendingRequest = m_requestQueue.dequeue();
    m_pendingRequest.transactionId = 0;
    m_hasPendingRequest = true;
    m_receiveBuffer.clear();

    if (m_socket->state() == QAbstractSocket::ConnectedState) {
        sendPendingRequest();
        return;
    }

    if (m_socket->state() != QAbstractSocket::ConnectingState) {
        m_socket->abort();
        m_socket->connectToHost(m_settings.host, static_cast<quint16>(m_settings.port));
    }
    m_requestTimeoutTimer->start(m_settings.requestTimeoutMs);
}

void GimbalControlClient::sendPendingRequest()
{
    if (!m_hasPendingRequest || m_socket->state() != QAbstractSocket::ConnectedState) {
        return;
    }

    m_pendingRequest.transactionId = nextTransactionId();
    QByteArray payload;
    switch (m_pendingRequest.type) {
    case RequestType::ReadStatus:
        payload = buildReadHoldingRequest(m_pendingRequest);
        break;
    case RequestType::WriteSingle:
        payload = buildWriteSingleRequest(m_pendingRequest);
        break;
    case RequestType::WriteMultiple:
        payload = buildWriteMultipleRequest(m_pendingRequest);
        break;
    }

    m_socket->write(payload);
    m_socket->flush();
    m_requestTimeoutTimer->start(m_settings.requestTimeoutMs);
    if (m_pendingRequest.type != RequestType::ReadStatus) {
        qCDebug(lcGimbalControlClient) << "Sent gimbal PLC request" << m_pendingRequest.operation;
    }
}

void GimbalControlClient::finishPendingRequest(bool success, const QString &message)
{
    if (!m_hasPendingRequest) {
        return;
    }

    const QString operation = m_pendingRequest.operation;
    m_requestTimeoutTimer->stop();
    m_hasPendingRequest = false;
    m_pendingRequest = Request{};

    if (success) {
        emit commandSucceeded(operation);
    } else {
        emit commandFailed(operation, message);
    }

    processNextRequest();
}

void GimbalControlClient::failPendingRequest(const QString &message)
{
    finishPendingRequest(false, message);
}

void GimbalControlClient::parsePendingResponse(const QByteArray &response)
{
    if (!m_hasPendingRequest) {
        return;
    }
    if (response.size() < 9) {
        failPendingRequest(tr("PLC 响应长度异常"));
        return;
    }

    const quint16 transactionId = readU16(response, 0);
    const quint16 protocolId = readU16(response, 2);
    const quint8 unitId = byteAt(response, 6);
    quint8 functionCode = byteAt(response, 7);

    if (transactionId != m_pendingRequest.transactionId || protocolId != kModbusProtocolId || unitId != m_settings.unitId) {
        failPendingRequest(tr("PLC 响应头不匹配"));
        return;
    }

    if ((functionCode & 0x80) != 0) {
        const quint8 exceptionCode = response.size() > 8 ? byteAt(response, 8) : 0;
        failPendingRequest(tr("PLC 异常响应，功能码=%1，异常码=%2").arg(functionCode).arg(exceptionCode));
        return;
    }

    switch (m_pendingRequest.type) {
    case RequestType::ReadStatus: {
        if (functionCode != kFunctionReadHolding || response.size() < 9) {
            failPendingRequest(tr("PLC 状态响应功能码异常"));
            return;
        }
        const int byteCount = byteAt(response, 8);
        if (byteCount < kMinimumStatusRegisterCount * 2 || response.size() < 9 + byteCount) {
            failPendingRequest(tr("PLC 状态响应寄存器数量不足"));
            return;
        }

        QVector<quint16> registers;
        registers.reserve(byteCount / 2);
        for (int i = 0; i + 1 < byteCount; i += 2) {
            registers.push_back(readU16(response, 9 + i));
        }

        GimbalStatus status;
        status.valid = true;
        status.state = registers.value(0);
        status.rawHeight = registers.value(1);
        status.height = toSigned16(registers.value(1));
        status.yaw = registers.value(2);
        status.pitch = registers.value(3);
        status.xMotion = registers.value(4);
        status.zMotion = registers.value(5);
        status.setVelocity = toSigned16(registers.value(6));
        emit statusReceived(status);
        finishPendingRequest(true);
        return;
    }
    case RequestType::WriteSingle:
        if (functionCode != kFunctionWriteSingle) {
            failPendingRequest(tr("PLC 单寄存器写入响应功能码异常"));
            return;
        }
        finishPendingRequest(true);
        return;
    case RequestType::WriteMultiple:
        if (functionCode != kFunctionWriteMultiple) {
            failPendingRequest(tr("PLC 多寄存器写入响应功能码异常"));
            return;
        }
        finishPendingRequest(true);
        return;
    }
}

QByteArray GimbalControlClient::buildReadHoldingRequest(const Request &request) const
{
    QByteArray data;
    appendU16(data, request.transactionId);
    appendU16(data, kModbusProtocolId);
    appendU16(data, 6);
    data.append(static_cast<char>(m_settings.unitId));
    data.append(static_cast<char>(kFunctionReadHolding));
    appendU16(data, request.address);
    appendU16(data, request.values.value(0));
    return data;
}

QByteArray GimbalControlClient::buildWriteSingleRequest(const Request &request) const
{
    QByteArray data;
    appendU16(data, request.transactionId);
    appendU16(data, kModbusProtocolId);
    appendU16(data, 6);
    data.append(static_cast<char>(m_settings.unitId));
    data.append(static_cast<char>(kFunctionWriteSingle));
    appendU16(data, request.address);
    appendU16(data, request.values.value(0));
    return data;
}

QByteArray GimbalControlClient::buildWriteMultipleRequest(const Request &request) const
{
    const quint16 quantity = static_cast<quint16>(request.values.size());
    const quint8 byteCount = static_cast<quint8>(quantity * 2);

    QByteArray data;
    appendU16(data, request.transactionId);
    appendU16(data, kModbusProtocolId);
    appendU16(data, static_cast<quint16>(7 + byteCount));
    data.append(static_cast<char>(m_settings.unitId));
    data.append(static_cast<char>(kFunctionWriteMultiple));
    appendU16(data, request.address);
    appendU16(data, quantity);
    data.append(static_cast<char>(byteCount));
    for (quint16 value : request.values) {
        appendU16(data, value);
    }
    return data;
}

quint16 GimbalControlClient::nextTransactionId()
{
    if (m_nextTransactionId == 0) {
        m_nextTransactionId = 1;
    }
    return m_nextTransactionId++;
}

quint16 GimbalControlClient::controlAddressForAxis(Axis axis) const
{
    switch (axis) {
    case Axis::Height:
        return static_cast<quint16>(m_settings.heightControlAddress);
    case Axis::Pitch:
        return static_cast<quint16>(m_settings.pitchControlAddress);
    case Axis::Yaw:
        return static_cast<quint16>(m_settings.yawControlAddress);
    }
    return 0;
}

bool GimbalControlClient::hasQueuedStatusRequest() const
{
    for (const Request &request : m_requestQueue) {
        if (request.type == RequestType::ReadStatus) {
            return true;
        }
    }
    return false;
}

int GimbalControlClient::toSigned16(quint16 value)
{
    return value > 32767 ? static_cast<int>(value) - 65536 : static_cast<int>(value);
}
