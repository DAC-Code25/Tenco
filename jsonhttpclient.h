#pragma once
#include <QHash>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QUrlQuery>
#include <functional>

class QNetworkReply;
struct JsonHttpResult {
    int status = 0;
    QJsonObject object;
    QString error;
    qint64 roundTripMs = 0;
    bool ok() const { return error.isEmpty() && status >= 200 && status < 300; }
};

// One bounded request per lane. Long polls never occupy the control/heartbeat lane.
class JsonHttpClient : public QObject {
    Q_OBJECT
  public:
    using Callback = std::function<void(const JsonHttpResult &)>;
    explicit JsonHttpClient(QObject *parent = nullptr);
    ~JsonHttpClient() override;
    void configure(const QUrl &baseUrl, const QString &bearer, int timeoutMs = 3000);
    bool configured() const;
    bool busy(const QString &lane) const { return m_pending.contains(lane); }
    bool request(const QString &lane, const QString &method, const QString &path, const QJsonObject &body,
                 const QUrlQuery &query, const QString &operatorToken, Callback callback,
                 qint64 maxResponseBytes = 1024 * 1024);
    void cancelAll();

  private:
    QNetworkAccessManager m_manager;
    QHash<QString, QPointer<QNetworkReply>> m_pending;
    QUrl m_base;
    QString m_bearer;
    int m_timeoutMs = 3000;
    quint64 m_generation = 0;
};
