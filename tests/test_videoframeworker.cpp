#include "../videoframeworker.h"

#include <QBuffer>
#include <QDir>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QNetworkReply>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <QtTest>

#define private public
#include "videoclient.h"
#undef private

class VideoFrameWorkerTest : public QObject
{
    Q_OBJECT

private slots:
    void decodesSingleJpegChunk();
    void decodesSplitJpegInput();
    void rateLimitsDisplayedFramesButKeepsLatestFrame();
    void recordingWritesOriginalJpegFrames();
    void resetClearsStateAndStopsRecording();
    void oldEpochBytesCannotCompleteANewStreamFrame();
    void queuedDecodedFramesCannotReviveDisconnectedStreams();
    void silentConnectionTimesOutAndReconnects_data();
    void silentConnectionTimesOutAndReconnects();
    void continuousFramesKeepTheTransferAlive();
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

    worker.enqueueBytes(makeJpeg(), 0);

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
    worker.enqueueBytes(jpeg.left(jpeg.size() / 2), 0);
    QCOMPARE(frameSpy.count(), 0);
    worker.enqueueBytes(jpeg.mid(jpeg.size() / 2), 0);

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

    worker.enqueueBytes(makeJpeg(8, 6, Qt::red), 0);
    worker.enqueueBytes(makeJpeg(9, 7, Qt::blue), 0);
    worker.enqueueBytes(makeJpeg(10, 8, Qt::yellow), 0);

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
    worker.enqueueBytes(first + second, 0);

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
    worker.enqueueBytes(makeJpeg(), 0);
    QVERIFY(worker.isRecording());
    QVERIFY(!worker.lastFrame().isNull());

    worker.reset();

    QVERIFY(!worker.isRecording());
    QVERIFY(worker.lastFrame().isNull());
    const auto metrics = worker.metrics();
    QCOMPARE(metrics.framesReceived, 0ULL);
    QCOMPARE(metrics.framesDecoded, 0ULL);
}

void VideoFrameWorkerTest::oldEpochBytesCannotCompleteANewStreamFrame()
{
    VideoFrameWorker worker;
    QSignalSpy frames(&worker, &VideoFrameWorker::frameReady);
    const auto oldImage = makeJpeg(8, 6, Qt::red);
    worker.resetStream(1);
    worker.enqueueBytes(oldImage.left(oldImage.size() / 2), 1);
    QVERIFY(frames.isEmpty());

    worker.resetStream(2);
    worker.enqueueBytes(oldImage.mid(oldImage.size() / 2), 1);
    worker.enqueueBytes(oldImage, 1);
    QVERIFY(frames.isEmpty());
    QVERIFY(worker.lastFrame().isNull());
    QCOMPARE(worker.metrics().bytesReceived, 0ULL);

    const auto newImage = makeJpeg(11, 9, Qt::blue);
    worker.enqueueBytes(newImage, 2);
    QCOMPARE(frames.size(), 1);
    QCOMPARE(frames.first()[0].value<QImage>().size(), QSize(11, 9));
    QCOMPARE(frames.first()[1].toULongLong(), 2ULL);
    QCOMPARE(worker.lastFrame().size(), QSize(11, 9));
    QCOMPARE(worker.metrics().framesDecoded, 1ULL);
}

void VideoFrameWorkerTest::queuedDecodedFramesCannotReviveDisconnectedStreams()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    connect(&server, &QTcpServer::newConnection, &server, [&server] {
        while (server.hasPendingConnections()) {
            auto *socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [socket] {
                socket->readAll();
                if (socket->property("replied").toBool()) return;
                socket->setProperty("replied", true);
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n");
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    });
    VideoClient client;
    client.setStreamUrl(QUrl(QStringLiteral("http://127.0.0.1:%1/stream").arg(server.serverPort())));
    client.setAutoReconnect(false);
    QSignalSpy frames(&client, &VideoClient::frameReceived);
    client.start();
    QVERIFY(client.isActive());
    const auto oldEpoch = client.m_streamEpoch;
    QImage oldFrame(8, 6, QImage::Format_RGB32);
    oldFrame.fill(Qt::red);
    // Emit on the decoder thread while the UI thread is blocked. Its queued
    // delivery must remain invalid even after a new HTTP reply is active.
    QVERIFY(QMetaObject::invokeMethod(client.m_frameWorker, [&client, oldFrame, oldEpoch] {
        emit client.m_frameWorker->frameReady(oldFrame, oldEpoch);
        emit client.m_frameWorker->workerError(QStringLiteral("late-old-error"), oldEpoch);
    }, Qt::BlockingQueuedConnection));
    client.handleStreamFinished();
    QVERIFY(!client.isActive());
    QVERIFY(client.m_lastFrame.isNull());
    client.start();
    QVERIFY(client.isActive());
    QVERIFY(client.m_streamEpoch != oldEpoch);
    QCoreApplication::sendPostedEvents(&client, QEvent::MetaCall);
    QVERIFY(frames.isEmpty());
    QVERIFY(client.m_lastFrame.isNull());
    QVERIFY(client.lastFrame().isNull());
    QVERIFY(client.m_state == VideoClient::State::Connecting);

    const auto currentEpoch = client.m_streamEpoch;
    const auto currentJpeg = makeJpeg(11, 9, Qt::blue);
    QVERIFY(QMetaObject::invokeMethod(client.m_frameWorker, [&client, currentJpeg, currentEpoch] {
        client.m_frameWorker->enqueueBytes(currentJpeg, currentEpoch);
    }, Qt::BlockingQueuedConnection));
    QCoreApplication::sendPostedEvents(&client, QEvent::MetaCall);
    QCOMPARE(frames.size(), 1);
    QCOMPARE(frames.first()[0].value<QImage>().size(), QSize(11, 9));
    QVERIFY(client.m_state == VideoClient::State::Streaming);
    client.stop();
    QVERIFY(client.lastFrame().isNull());
}

void VideoFrameWorkerTest::silentConnectionTimesOutAndReconnects_data()
{
    QTest::addColumn<bool>("sendHeader");
    QTest::newRow("tcp-without-http-response") << false;
    QTest::newRow("http-stream-without-body") << true;
}

void VideoFrameWorkerTest::silentConnectionTimesOutAndReconnects()
{
    QFETCH(bool, sendHeader);
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    int connections = 0;
    connect(&server, &QTcpServer::newConnection, &server, [&server, &connections, sendHeader] {
        while (server.hasPendingConnections()) {
            auto *socket = server.nextPendingConnection();
            ++connections;
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            // First connection stays silent, either before or after HTTP headers.
            if (connections == 1) {
                if (sendHeader) connect(socket, &QTcpSocket::readyRead, socket, [socket] {
                    socket->readAll();
                    if (socket->property("replied").toBool()) return;
                    socket->setProperty("replied", true);
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n");
                });
                continue;
            }
            connect(socket, &QTcpSocket::readyRead, socket, [socket] {
                socket->readAll();
                if (socket->property("replied").toBool()) return;
                socket->setProperty("replied", true);
                const auto jpeg = makeJpeg(11, 9, Qt::blue);
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n");
                socket->write("--frame\r\nContent-Type: image/jpeg\r\nContent-Length: " +
                    QByteArray::number(jpeg.size()) + "\r\n\r\n" + jpeg + "\r\n");
            });
        }
    });
    VideoClient client;
    client.m_transferTimeoutMs = 150;
    client.setReconnectIntervalMs(200);
    client.setStreamUrl(QUrl(QStringLiteral("http://127.0.0.1:%1/stream").arg(server.serverPort())));
    QSignalSpy states(&client, &VideoClient::stateChanged);
    QSignalSpy frames(&client, &VideoClient::frameReceived);
    client.start();
    QTRY_VERIFY_WITH_TIMEOUT(!frames.isEmpty(), 5000);
    QVERIFY(connections >= 2);
    bool sawTimeout = false;
    for (const auto &event : states) {
        if (event[0].value<VideoClient::State>() == VideoClient::State::Error &&
            event[1].toString().contains(QStringLiteral("未收到数据"))) sawTimeout = true;
    }
    QVERIFY(sawTimeout);
    client.stop();
}

void VideoFrameWorkerTest::continuousFramesKeepTheTransferAlive()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    connect(&server, &QTcpServer::newConnection, &server, [&server] {
        while (server.hasPendingConnections()) {
            auto *socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QTcpSocket::readyRead, socket, [socket] {
                socket->readAll();
                if (socket->property("replied").toBool()) return;
                socket->setProperty("replied", true);
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n");
                const auto jpeg = makeJpeg(11, 9, Qt::blue);
                const auto part = "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: " +
                    QByteArray::number(jpeg.size()) + "\r\n\r\n" + jpeg + "\r\n";
                auto *timer = new QTimer(socket);
                timer->setInterval(40);
                connect(timer, &QTimer::timeout, socket, [socket, part] { socket->write(part); });
                timer->start();
                socket->write(part);
            });
        }
    });
    VideoClient client;
    client.m_transferTimeoutMs = 250;
    client.setAutoReconnect(false);
    client.setStreamUrl(QUrl(QStringLiteral("http://127.0.0.1:%1/stream").arg(server.serverPort())));
    QSignalSpy states(&client, &VideoClient::stateChanged);
    QSignalSpy frames(&client, &VideoClient::frameReceived);
    client.start();
    QTRY_VERIFY_WITH_TIMEOUT(!frames.isEmpty(), 3000);
    const auto before = frames.size();
    QTest::qWait(800); // More than three idle limits, with bytes arriving throughout.
    QVERIFY(client.isActive());
    QVERIFY(frames.size() > before);
    for (const auto &event : states)
        QVERIFY(event[0].value<VideoClient::State>() != VideoClient::State::Error);
    client.stop();
}

QTEST_MAIN(VideoFrameWorkerTest)
#include "test_videoframeworker.moc"
