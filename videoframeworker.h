#ifndef VIDEOFRAMEWORKER_H
#define VIDEOFRAMEWORKER_H

#include <QObject>

#include <QByteArray>
#include <QImage>
#include <QMetaType>
#include <QMutex>
#include <QString>

class QFile;

class VideoFrameWorker : public QObject
{
    Q_OBJECT

public:
    struct Settings {
        int maxDisplayFps = 30;
        int maxBufferBytes = 3 * 1024 * 1024;
    };

    struct Metrics {
        quint64 bytesReceived = 0;
        quint64 framesReceived = 0;
        quint64 framesDecoded = 0;
        quint64 framesDisplayed = 0;
        quint64 framesDropped = 0;
        quint64 decodeFailures = 0;
        quint64 recordingFrames = 0;
        double averageDecodeMs = 0.0;
        qint64 lastFrameEpochMs = 0;
    };

    explicit VideoFrameWorker(QObject *parent = nullptr);
    ~VideoFrameWorker() override;

    void configure(const Settings &settings);
    Settings settings() const;

    bool startRecording(const QString &directory, QString *outPath = nullptr);
    QString stopRecording();
    bool isRecording() const;

    QImage lastFrame() const;
    Metrics metrics() const;

public slots:
    void enqueueBytes(const QByteArray &chunk);
    void resetStream();
    void reset();

signals:
    void frameReady(const QImage &frame);
    void metricsUpdated(const VideoFrameWorker::Metrics &metrics);
    void workerError(const QString &message);

private:
    static constexpr int kMinDisplayFps = 1;
    static constexpr int kMaxDisplayFps = 120;
    static constexpr int kMinBufferBytes = 64 * 1024;
    static constexpr int kMaxBufferBytesLimit = 16 * 1024 * 1024;

    void processBuffer();
    void handleJpegFrame(const QByteArray &frameData);
    bool shouldDisplayFrame(qint64 nowMs) const;
    void writeRecordingFrame(const QByteArray &frameData);
    void emitMetrics();

    mutable QMutex m_mutex;
    Settings m_settings;
    Metrics m_metrics;
    QByteArray m_buffer;
    QImage m_lastFrame;
    QFile *m_recordFile = nullptr;
    QString m_recordFilePath;
    bool m_recording = false;
    qint64 m_lastDisplayMs = 0;
};

Q_DECLARE_METATYPE(VideoFrameWorker::Metrics)

#endif // VIDEOFRAMEWORKER_H
