#include "oakcameravideosource.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QSize>
#include <QStringList>
#include <QThread>

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

#if defined(TENCO_ENABLE_OAK_CAMERA)
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/videoio.hpp>

#include <depthai/depthai.hpp>
#endif

Q_LOGGING_CATEGORY(lcOakVideoSource, "tenco.video.oak")

namespace {

#if defined(TENCO_ENABLE_OAK_CAMERA)
QString timestampForFileName()
{
    return QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_hhmmss"));
}

QString ensureDirectoryAndBuildPath(const QString &directory, const QString &prefix, const QString &extension)
{
    if (directory.trimmed().isEmpty()) {
        return QString();
    }

    QDir dir(directory);
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        return QString();
    }

    return dir.filePath(QStringLiteral("%1_%2.%3").arg(prefix, timestampForFileName(), extension));
}

QString normalizeFourcc(const QString &codec)
{
    const QString value = codec.trimmed().toUpper();
    if (value.isEmpty()) {
        return QStringLiteral("MJPG");
    }
    if (value.size() >= 4) {
        return value.left(4);
    }
    return value.leftJustified(4, QLatin1Char(' '));
}

int fourccFromString(const QString &codec)
{
    const QByteArray fourcc = normalizeFourcc(codec).toLatin1();
    return cv::VideoWriter::fourcc(
        fourcc.value(0, 'M'),
        fourcc.value(1, 'J'),
        fourcc.value(2, 'P'),
        fourcc.value(3, 'G'));
}

QImage matToQImage(const cv::Mat &bgrFrame)
{
    if (bgrFrame.empty()) {
        return QImage();
    }

    cv::Mat rgbFrame;
    cv::cvtColor(bgrFrame, rgbFrame, cv::COLOR_BGR2RGB);
    return QImage(rgbFrame.data,
                  rgbFrame.cols,
                  rgbFrame.rows,
                  static_cast<int>(rgbFrame.step),
                  QImage::Format_RGB888)
        .copy();
}

dai::ColorCameraProperties::SensorResolution pickSensorResolution(const ConfigManager::VideoConfig &config)
{
    Q_UNUSED(config);
    // OAK-D-Pro-W RGB sensor is 12MP. Keep the sensor at native resolution so still
    // captures use the full-resolution path while preview is downscaled separately.
    return dai::ColorCameraProperties::SensorResolution::THE_12_MP;
}

QString usbSpeedToString(dai::UsbSpeed speed)
{
    switch (speed) {
    case dai::UsbSpeed::LOW:
        return QStringLiteral("LOW");
    case dai::UsbSpeed::FULL:
        return QStringLiteral("FULL");
    case dai::UsbSpeed::HIGH:
        return QStringLiteral("HIGH");
    case dai::UsbSpeed::SUPER:
        return QStringLiteral("SUPER");
    case dai::UsbSpeed::SUPER_PLUS:
        return QStringLiteral("SUPER_PLUS");
    case dai::UsbSpeed::UNKNOWN:
    default:
        return QStringLiteral("UNKNOWN");
    }
}
#endif

} // namespace

class OakCameraVideoSource::Impl
{
public:
    explicit Impl(OakCameraVideoSource *owner, const ConfigManager::VideoConfig &videoConfig)
        : q(owner)
        , config(videoConfig)
    {
    }

    ~Impl()
    {
        shutdown();
    }

    void setReconnectIntervalMs(int intervalMs)
    {
        reconnectIntervalMs = qMax(200, intervalMs);
    }

    void setAutoReconnect(bool enabled)
    {
        autoReconnect = enabled;
    }

    bool isConfigured() const
    {
        return config.backend == QStringLiteral("oak_depthai");
    }

    bool isActive() const
    {
        return active.load();
    }

    bool isRecording() const
    {
        QMutexLocker locker(&mutex);
        return recording;
    }

    QImage lastFrame() const
    {
        QMutexLocker locker(&mutex);
        return lastFrameImage;
    }

    void start()
    {
#if !defined(TENCO_ENABLE_OAK_CAMERA)
        emitState(AbstractVideoSource::State::Error,
                  OakCameraVideoSource::tr("OAK 相机后端未启用：请使用 -DTENCO_ENABLE_OAK_CAMERA=ON 并安装 DepthAI/OpenCV"));
        return;
#else
        if (!isConfigured()) {
            emitState(AbstractVideoSource::State::Error,
                      OakCameraVideoSource::tr("当前视频后端不是 oak_depthai，无法启动 OAK 相机"));
            return;
        }

        if (worker.joinable()) {
            worker.join();
        }

        bool expected = false;
        if (!active.compare_exchange_strong(expected, true)) {
            return;
        }

        stopRequested.store(false);
        worker = std::thread([this]() { workerLoop(); });
#endif
    }

    void stop()
    {
        shutdown();
    }

    bool startRecording(const QString &directory, QString *outPath)
    {
        if (outPath) {
            outPath->clear();
        }

#if !defined(TENCO_ENABLE_OAK_CAMERA)
        Q_UNUSED(directory);
        emitState(AbstractVideoSource::State::Error,
                  OakCameraVideoSource::tr("OAK 相机后端未启用：无法开始录像"));
        return false;
#else
        const QString fullPath = ensureDirectoryAndBuildPath(directory, QStringLiteral("oak_video"), QStringLiteral("avi"));
        if (fullPath.isEmpty()) {
            emitState(AbstractVideoSource::State::Error,
                      OakCameraVideoSource::tr("录像目录不可用，无法创建录像文件"));
            return false;
        }

        cv::Mat frameForWriter;
        {
            QMutexLocker locker(&mutex);
            if (recording) {
                if (outPath) {
                    *outPath = recordFilePath;
                }
                return true;
            }

            if (lastFrameMat.empty()) {
                emitState(AbstractVideoSource::State::Error,
                          OakCameraVideoSource::tr("当前还没有可用的视频帧，无法开始录像"));
                return false;
            }

            frameForWriter = lastFrameMat.clone();
        }

        cv::VideoWriter writer;
        const int fourcc = fourccFromString(config.recordCodec);
        const double fps = qMax(1, config.previewFps);
        const cv::Size frameSize(frameForWriter.cols, frameForWriter.rows);
        if (!writer.open(fullPath.toStdString(), fourcc, fps, frameSize, true)) {
            emitState(AbstractVideoSource::State::Error,
                      OakCameraVideoSource::tr("无法打开录像文件：%1").arg(fullPath));
            return false;
        }

        {
            QMutexLocker locker(&mutex);
            videoWriter = std::move(writer);
            recordFilePath = fullPath;
            recording = true;
        }

        if (outPath) {
            *outPath = fullPath;
        }

        qCInfo(lcOakVideoSource) << "Recording started:" << fullPath;
        emitState(AbstractVideoSource::State::Streaming,
                  OakCameraVideoSource::tr("OAK 录像已开始：%1").arg(QFileInfo(fullPath).fileName()));
        return true;
#endif
    }

    QString stopRecording()
    {
#if !defined(TENCO_ENABLE_OAK_CAMERA)
        return QString();
#else
        QMutexLocker locker(&mutex);
        const QString savedPath = recordFilePath;
        if (videoWriter.isOpened()) {
            videoWriter.release();
        }
        recording = false;
        recordFilePath.clear();
        if (!savedPath.isEmpty()) {
            qCInfo(lcOakVideoSource) << "Recording stopped:" << savedPath;
        }
        return savedPath;
#endif
    }

    bool saveSnapshot(const QString &directory, QString *outPath) const
    {
        if (outPath) {
            outPath->clear();
        }

#if !defined(TENCO_ENABLE_OAK_CAMERA)
        Q_UNUSED(directory);
        return false;
#else
        try {
            const QString fullPath = ensureDirectoryAndBuildPath(directory, QStringLiteral("oak_photo"), QStringLiteral("jpg"));
            if (fullPath.isEmpty()) {
                return false;
            }

            auto stillFrame = requestStillFrame();
            if (!stillFrame) {
                qCWarning(lcOakVideoSource) << "Still frame request failed, fallback to last preview frame";
                QMutexLocker locker(&mutex);
                if (lastFrameImage.isNull()) {
                    return false;
                }

                if (!lastFrameImage.save(fullPath, "JPG", 95)) {
                    return false;
                }
            } else {
                const cv::Mat bgrFrame = stillFrame->getCvFrame();
                if (bgrFrame.empty()) {
                    return false;
                }

                if (!cv::imwrite(fullPath.toStdString(), bgrFrame)) {
                    return false;
                }
            }

            if (outPath) {
                *outPath = fullPath;
            }
            qCInfo(lcOakVideoSource) << "Snapshot saved:" << fullPath;
            return true;
        } catch (const std::exception &e) {
            qCWarning(lcOakVideoSource) << "Snapshot failed:" << e.what();
            return false;
        }
#endif
    }

private:
    OakCameraVideoSource *q = nullptr;
    ConfigManager::VideoConfig config;

    mutable QMutex mutex;
    QImage lastFrameImage;
#if defined(TENCO_ENABLE_OAK_CAMERA)
    cv::Mat lastFrameMat;
    cv::VideoWriter videoWriter;
    std::shared_ptr<dai::Device> device;
    std::shared_ptr<dai::DataOutputQueue> previewQueue;
    std::shared_ptr<dai::DataOutputQueue> stillQueue;
    std::shared_ptr<dai::DataInputQueue> controlQueue;
#endif

    std::thread worker;
    std::atomic_bool active{false};
    std::atomic_bool stopRequested{false};
    bool recording = false;
    QString recordFilePath;
    int reconnectIntervalMs = 2000;
    bool autoReconnect = true;

    void emitState(AbstractVideoSource::State state, const QString &message) const
    {
        QMetaObject::invokeMethod(
            q,
            [this, state, message]() {
                emit q->stateChanged(state, message);
            },
            Qt::QueuedConnection);
    }

    void emitFrame(const QImage &image)
    {
        QMetaObject::invokeMethod(
            q,
            [this, image]() {
                emit q->frameReceived(image);
            },
            Qt::QueuedConnection);
    }

    void shutdown()
    {
        stopRequested.store(true);

        if (worker.joinable()) {
            worker.join();
        }

#if defined(TENCO_ENABLE_OAK_CAMERA)
        {
            QMutexLocker locker(&mutex);
            if (videoWriter.isOpened()) {
                videoWriter.release();
            }
            recording = false;
            recordFilePath.clear();
            controlQueue.reset();
            previewQueue.reset();
            stillQueue.reset();
            if (device) {
                try {
                    device->close();
                } catch (const std::exception &e) {
                    qCWarning(lcOakVideoSource) << "Failed to close OAK device cleanly:" << e.what();
                }
            }
            device.reset();
            lastFrameMat.release();
        }
#endif

        {
            QMutexLocker locker(&mutex);
            lastFrameImage = QImage();
        }

        if (active.exchange(false)) {
            emitState(AbstractVideoSource::State::Stopped, OakCameraVideoSource::tr("OAK 相机已停止"));
        }
    }

#if defined(TENCO_ENABLE_OAK_CAMERA)
    dai::Pipeline createPipeline() const
    {
        dai::Pipeline pipeline;

        auto camRgb = pipeline.create<dai::node::ColorCamera>();
        auto previewOut = pipeline.create<dai::node::XLinkOut>();
        auto stillOut = pipeline.create<dai::node::XLinkOut>();
        auto controlIn = pipeline.create<dai::node::XLinkIn>();

        previewOut->setStreamName("preview");
        stillOut->setStreamName("still");
        controlIn->setStreamName("control");

        camRgb->setBoardSocket(dai::CameraBoardSocket::CAM_A);
        camRgb->setResolution(pickSensorResolution(config));
        camRgb->setPreviewSize(config.previewWidth, config.previewHeight);
        camRgb->setStillSize(4032, 3040);
        camRgb->setInterleaved(false);
        camRgb->setColorOrder(dai::ColorCameraProperties::ColorOrder::BGR);
        camRgb->setPreviewKeepAspectRatio(true);
        camRgb->setFps(static_cast<float>(qMax(1, config.previewFps)));

        camRgb->preview.link(previewOut->input);
        camRgb->still.link(stillOut->input);
        controlIn->out.link(camRgb->inputControl);

        return pipeline;
    }

    std::shared_ptr<dai::Device> createDevice(const dai::Pipeline &pipeline) const
    {
        if (!config.deviceId.isEmpty()) {
            auto [found, info] = dai::Device::getDeviceByMxId(config.deviceId.toStdString());
            if (!found) {
                throw std::runtime_error(
                    QStringLiteral("未找到指定 OAK 设备：%1").arg(config.deviceId).toStdString());
            }
            return std::make_shared<dai::Device>(pipeline, info, dai::UsbSpeed::SUPER);
        }

        auto [found, info] = dai::Device::getAnyAvailableDevice();
        if (!found) {
            throw std::runtime_error("未检测到可用的 OAK 设备");
        }
        return std::make_shared<dai::Device>(pipeline, info, dai::UsbSpeed::SUPER);
    }

    void initializeDevice()
    {
        auto pipeline = createPipeline();
        auto newDevice = createDevice(pipeline);
        auto newPreviewQueue = newDevice->getOutputQueue("preview", 4, false);
        auto newStillQueue = newDevice->getOutputQueue("still", 2, false);
        auto newControlQueue = newDevice->getInputQueue("control", 2, false);

        {
            QMutexLocker locker(&mutex);
            device = std::move(newDevice);
            previewQueue = std::move(newPreviewQueue);
            stillQueue = std::move(newStillQueue);
            controlQueue = std::move(newControlQueue);
        }

        qCInfo(lcOakVideoSource)
            << "OAK device ready, MXID:"
            << QString::fromStdString(device->getMxId())
            << ", USB speed:"
            << usbSpeedToString(device->getUsbSpeed());

        emitState(AbstractVideoSource::State::Connecting,
                  OakCameraVideoSource::tr("OAK 相机已连接，正在启动预览..."));
    }

    void workerLoop()
    {
        while (!stopRequested.load()) {
            try {
                initializeDevice();
                emitState(AbstractVideoSource::State::Streaming,
                          OakCameraVideoSource::tr("OAK 相机预览已启动"));
                previewLoop();
                if (stopRequested.load()) {
                    break;
                }

                emitState(AbstractVideoSource::State::Error,
                          OakCameraVideoSource::tr("OAK 相机预览已中断，准备重连..."));
            } catch (const std::exception &e) {
                qCWarning(lcOakVideoSource) << "OAK worker exception:" << e.what();
                emitState(AbstractVideoSource::State::Error,
                          OakCameraVideoSource::tr("OAK 相机异常：%1").arg(QString::fromUtf8(e.what())));
            }

            releaseDeviceResources();

            if (!autoReconnect || stopRequested.load()) {
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(reconnectIntervalMs));
        }

        active.store(false);
    }

    void releaseDeviceResources()
    {
        QMutexLocker locker(&mutex);
        controlQueue.reset();
        previewQueue.reset();
        stillQueue.reset();
        if (device) {
            try {
                device->close();
            } catch (const std::exception &e) {
                qCWarning(lcOakVideoSource) << "Release device error:" << e.what();
            }
        }
        device.reset();
    }

    void previewLoop()
    {
        while (!stopRequested.load()) {
            std::shared_ptr<dai::DataOutputQueue> queue;
            {
                QMutexLocker locker(&mutex);
                queue = previewQueue;
            }

            if (!queue) {
                throw std::runtime_error("预览输出队列未初始化");
            }

            bool hasTimedOut = false;
            auto frame = queue->get<dai::ImgFrame>(std::chrono::milliseconds(250), hasTimedOut);
            if (hasTimedOut || !frame) {
                continue;
            }

            const cv::Mat bgrFrame = frame->getCvFrame();
            if (bgrFrame.empty()) {
                continue;
            }

            const QImage image = matToQImage(bgrFrame);
            {
                QMutexLocker locker(&mutex);
                lastFrameMat = bgrFrame.clone();
                lastFrameImage = image;

                if (recording) {
                    if (!videoWriter.isOpened()) {
                        throw std::runtime_error("录像器已失效");
                    }
                    videoWriter.write(lastFrameMat);
                }
            }

            if (!image.isNull()) {
                emitFrame(image);
            }
        }
    }

    std::shared_ptr<dai::ImgFrame> requestStillFrame() const
    {
        std::shared_ptr<dai::DataInputQueue> inputQueue;
        std::shared_ptr<dai::DataOutputQueue> outputQueue;
        {
            QMutexLocker locker(&mutex);
            inputQueue = controlQueue;
            outputQueue = stillQueue;
        }

        if (!inputQueue || !outputQueue) {
            return nullptr;
        }

        dai::CameraControl control;
        control.setCaptureStill(true);
        inputQueue->send(control);

        bool hasTimedOut = false;
        return outputQueue->get<dai::ImgFrame>(std::chrono::seconds(3), hasTimedOut);
    }
#endif
};

OakCameraVideoSource::OakCameraVideoSource(const ConfigManager::VideoConfig &config, QObject *parent)
    : AbstractVideoSource(parent)
    , m_impl(std::make_unique<Impl>(this, config))
{
}

OakCameraVideoSource::~OakCameraVideoSource()
{
    stop();
}

void OakCameraVideoSource::start()
{
    m_impl->start();
}

void OakCameraVideoSource::stop()
{
    m_impl->stop();
}

bool OakCameraVideoSource::isActive() const
{
    return m_impl->isActive();
}

bool OakCameraVideoSource::isConfigured() const
{
    return m_impl->isConfigured();
}

bool OakCameraVideoSource::startRecording(const QString &directory, QString *outPath)
{
    return m_impl->startRecording(directory, outPath);
}

QString OakCameraVideoSource::stopRecording()
{
    return m_impl->stopRecording();
}

bool OakCameraVideoSource::isRecording() const
{
    return m_impl->isRecording();
}

bool OakCameraVideoSource::saveSnapshot(const QString &directory, QString *outPath) const
{
    return m_impl->saveSnapshot(directory, outPath);
}

QImage OakCameraVideoSource::lastFrame() const
{
    return m_impl->lastFrame();
}

void OakCameraVideoSource::setReconnectIntervalMs(int intervalMs)
{
    m_impl->setReconnectIntervalMs(intervalMs);
}

void OakCameraVideoSource::setAutoReconnect(bool enabled)
{
    m_impl->setAutoReconnect(enabled);
}
