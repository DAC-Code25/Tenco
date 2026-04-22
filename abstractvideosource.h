#ifndef ABSTRACTVIDEOSOURCE_H
#define ABSTRACTVIDEOSOURCE_H

#include <QObject>

#include <QImage>
#include <QString>
#include <QUrl>

class AbstractVideoSource : public QObject
{
    Q_OBJECT

public:
    enum class State {
        Stopped,
        Connecting,
        Streaming,
        Error
    };
    Q_ENUM(State)

    explicit AbstractVideoSource(QObject *parent = nullptr);
    ~AbstractVideoSource() override;

    virtual void start() = 0;
    virtual void stop() = 0;
    virtual bool isActive() const = 0;
    virtual bool isConfigured() const = 0;

    virtual bool startRecording(const QString &directory, QString *outPath = nullptr) = 0;
    virtual QString stopRecording() = 0;
    virtual bool isRecording() const = 0;

    virtual bool saveSnapshot(const QString &directory, QString *outPath = nullptr) const = 0;
    virtual QImage lastFrame() const = 0;

    virtual void setStreamUrlTemplate(const QString &templateUrl);
    virtual QString buildUrlForTopic(const QString &topic) const;
    virtual void setStreamUrl(const QUrl &url);
    virtual QUrl streamUrl() const;
    virtual void setReconnectIntervalMs(int intervalMs);
    virtual void setAutoReconnect(bool enabled);
    virtual bool supportsTopics() const;

signals:
    void frameReceived(const QImage &frame);
    void stateChanged(AbstractVideoSource::State state, const QString &message);
};

#endif // ABSTRACTVIDEOSOURCE_H
