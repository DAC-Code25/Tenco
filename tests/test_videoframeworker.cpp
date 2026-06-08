#include "../videoframeworker.h"

#include <QBuffer>
#include <QDir>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <QtTest>

class VideoFrameWorkerTest : public QObject
{
    Q_OBJECT

private slots:
    void decodesSingleJpegChunk();
    void decodesSplitJpegInput();
    void rateLimitsDisplayedFramesButKeepsLatestFrame();
    void recordingWritesOriginalJpegFrames();
    void resetClearsStateAndStopsRecording();
};

namespace {

QByteArray makeJpeg(int width = 8, int height = 6, QColor color = Qt::red)
{
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(color);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPG", 90);
    return bytes;
}

} // namespace

void VideoFrameWorkerTest::decodesSingleJpegChunk()
{
    VideoFrameWorker worker;
    QSignalSpy frameSpy(&worker, &VideoFrameWorker::frameReady);
    QVERIFY(frameSpy.isValid());

    worker.enqueueBytes(makeJpeg());

    QCOMPARE(frameSpy.count(), 1);
    const QImage frame = frameSpy.takeFirst().at(0).value<QImage>();
    QCOMPARE(frame.size(), QSize(8, 6));

    const auto metrics = worker.metrics();
    QCOMPARE(metrics.framesReceived, 1ULL);
    QCOMPARE(metrics.framesDecoded, 1ULL);
    QCOMPARE(metrics.framesDisplayed, 1ULL);
    QCOMPARE(metrics.decodeFailures, 0ULL);
    QVERIFY(metrics.averageDecodeMs >= 0.0);
}

void VideoFrameWorkerTest::decodesSplitJpegInput()
{
    VideoFrameWorker worker;
    QSignalSpy frameSpy(&worker, &VideoFrameWorker::frameReady);
    QVERIFY(frameSpy.isValid());

    const QByteArray jpeg = makeJpeg(10, 7, Qt::green);
    worker.enqueueBytes(jpeg.left(jpeg.size() / 2));
    QCOMPARE(frameSpy.count(), 0);
    worker.enqueueBytes(jpeg.mid(jpeg.size() / 2));

    QCOMPARE(frameSpy.count(), 1);
    QCOMPARE(worker.lastFrame().size(), QSize(10, 7));
    QCOMPARE(worker.metrics().framesDecoded, 1ULL);
}

void VideoFrameWorkerTest::rateLimitsDisplayedFramesButKeepsLatestFrame()
{
    VideoFrameWorker worker;
    VideoFrameWorker::Settings settings;
    settings.maxDisplayFps = 1;
    worker.configure(settings);

    QSignalSpy frameSpy(&worker, &VideoFrameWorker::frameReady);
    QVERIFY(frameSpy.isValid());

    worker.enqueueBytes(makeJpeg(8, 6, Qt::red));
    worker.enqueueBytes(makeJpeg(9, 7, Qt::blue));
    worker.enqueueBytes(makeJpeg(10, 8, Qt::yellow));

    QCOMPARE(frameSpy.count(), 1);
    QCOMPARE(worker.lastFrame().size(), QSize(10, 8));

    const auto metrics = worker.metrics();
    QCOMPARE(metrics.framesReceived, 3ULL);
    QCOMPARE(metrics.framesDecoded, 3ULL);
    QCOMPARE(metrics.framesDisplayed, 1ULL);
    QCOMPARE(metrics.framesDropped, 2ULL);
}

void VideoFrameWorkerTest::recordingWritesOriginalJpegFrames()
{
    VideoFrameWorker worker;
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    QString path;
    QVERIFY(worker.startRecording(dir.path(), &path));
    QVERIFY(QFile::exists(path));

    const QByteArray first = makeJpeg(8, 6, Qt::red);
    const QByteArray second = makeJpeg(8, 6, Qt::blue);
    worker.enqueueBytes(first + second);

    const QString savedPath = worker.stopRecording();
    QCOMPARE(savedPath, path);
    QFile file(savedPath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray recorded = file.readAll();
    QCOMPARE(recorded, first + second);
    QCOMPARE(worker.metrics().recordingFrames, 2ULL);
}

void VideoFrameWorkerTest::resetClearsStateAndStopsRecording()
{
    VideoFrameWorker worker;
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    QString path;
    QVERIFY(worker.startRecording(dir.path(), &path));
    worker.enqueueBytes(makeJpeg());
    QVERIFY(worker.isRecording());
    QVERIFY(!worker.lastFrame().isNull());

    worker.reset();

    QVERIFY(!worker.isRecording());
    QVERIFY(worker.lastFrame().isNull());
    const auto metrics = worker.metrics();
    QCOMPARE(metrics.framesReceived, 0ULL);
    QCOMPARE(metrics.framesDecoded, 0ULL);
}

QTEST_MAIN(VideoFrameWorkerTest)
#include "test_videoframeworker.moc"
