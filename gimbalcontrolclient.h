#ifndef GIMBALCONTROLCLIENT_H
#define GIMBALCONTROLCLIENT_H

#include <QObject>
#include <QQueue>
#include <QString>
#include <QVector>
#include <QtGlobal>

class QTcpSocket;
class QTimer;

struct GimbalStatus {
    bool valid = false;
    int state = 0;
    int height = 0;
    int rawHeight = 0;
    int yaw = 0;
    int pitch = 0;
    int xMotion = 0;
    int zMotion = 0;
    int setVelocity = 0;
};

class GimbalControlClient : public QObject
{
    Q_OBJECT

public:
    enum class Axis {
        Height,
        Pitch,
        Yaw
    };

    enum class Direction {
        Value1 = 1,
        Value2 = 2
    };

    struct Settings {
        bool enabled = false;
        QString host;
        int port = 502;
        int unitId = 255;
        int requestTimeoutMs = 1000;
        int statusPollIntervalMs = 300;
        int heightControlAddress = 0;
        int pitchControlAddress = 1;
        int yawControlAddress = 2;
        int statusStartAddress = 100;
        int statusRegisterCount = 7;
    };

    explicit GimbalControlClient(QObject *parent = nullptr);
    ~GimbalControlClient() override;

    void configure(const Settings &settings);
    bool isConfigured() const;
    bool isBusy() const { return m_hasPendingRequest; }

    void startStatusPolling();
    void stopStatusPolling();
    void requestStatus();

    void jog(Axis axis, Direction direction);
    void stopAxis(Axis axis);
    void stopAll();

    static QString axisName(Axis axis);
    static QString directionName(Direction direction);

signals:
    void connectionStateChanged(bool connected);
    void statusReceived(const GimbalStatus &status);
    void commandSucceeded(const QString &operation);
    void commandFailed(const QString &operation, const QString &message);

private slots:
    void handleSocketConnected();
    void handleSocketDisconnected();
    void handleSocketError();
    void handleSocketReadyRead();
    void handleRequestTimeout();
    void handleStatusPollTimeout();

private:
    enum class RequestType {
        ReadStatus,
        WriteSingle,
        WriteMultiple
    };

    struct Request {
        RequestType type = RequestType::ReadStatus;
        quint16 address = 0;
        QVector<quint16> values;
        QString operation;
        quint16 transactionId = 0;
    };

    void enqueueRequest(const Request &request, bool urgent = false);
    void processNextRequest();
    void sendPendingRequest();
    void finishPendingRequest(bool success, const QString &message = QString());
    void failPendingRequest(const QString &message);
    void parsePendingResponse(const QByteArray &response);
    QByteArray buildReadHoldingRequest(const Request &request) const;
    QByteArray buildWriteSingleRequest(const Request &request) const;
    QByteArray buildWriteMultipleRequest(const Request &request) const;
    quint16 nextTransactionId();
    quint16 controlAddressForAxis(Axis axis) const;
    bool hasQueuedStatusRequest() const;
    static int toSigned16(quint16 value);

    Settings m_settings;
    QTcpSocket *m_socket = nullptr;
    QTimer *m_requestTimeoutTimer = nullptr;
    QTimer *m_statusPollTimer = nullptr;
    QQueue<Request> m_requestQueue;
    Request m_pendingRequest;
    QByteArray m_receiveBuffer;
    quint16 m_nextTransactionId = 1;
    bool m_hasPendingRequest = false;
};

#endif // GIMBALCONTROLCLIENT_H
