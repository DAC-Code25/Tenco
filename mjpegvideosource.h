#ifndef MJPEGVIDEOSOURCE_H
#define MJPEGVIDEOSOURCE_H

#include "abstractvideosource.h"

#include "videoclient.h"

class MjpegVideoSource : public AbstractVideoSource
{
    Q_OBJECT

public:
    explicit MjpegVideoSource(QObject *parent = nullptr);

    void start() override;
    void stop() override;
    bool isActive() const override;
    bool isConfigured() const override;

    bool startRecording(const QString &directory, QString *outPath = nullptr) override;
    QString stopRecording() override;
    bool isRecording() const override;

    bool saveSnapshot(const QString &directory, QString *outPath = nullptr) const override;
    QImage lastFrame() const override;

    void setStreamUrlTemplate(const QString &templateUrl) override;
    QString buildUrlForTopic(const QString &topic) const override;
    void setStreamUrl(const QUrl &url) override;
    QUrl streamUrl() const override;
    void setReconnectIntervalMs(int intervalMs) override;
    void setAutoReconnect(bool enabled) override;
    bool supportsTopics() const override;

private:
    static AbstractVideoSource::State mapState(VideoClient::State state);

    VideoClient m_client;
};

#endif // MJPEGVIDEOSOURCE_H
