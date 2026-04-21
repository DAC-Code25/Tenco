#include "oakcameravideosource.h"

#include <QDir>

OakCameraVideoSource::OakCameraVideoSource(const ConfigManager::VideoConfig &config, QObject *parent)
    : AbstractVideoSource(parent)
    , m_config(config)
{
}

OakCameraVideoSource::~OakCameraVideoSource()
{
    stop();
}

void OakCameraVideoSource::start()
{
#if defined(TENCO_ENABLE_OAK_CAMERA)
    m_active = true;
    emit stateChanged(State::Error, tr("OAK 相机后端尚未完成 DepthAI 采集实现"));
#else
    m_active = false;
    emit stateChanged(State::Error, tr("OAK 相机后端未启用：请使用 -DTENCO_ENABLE_OAK_CAMERA=ON 并安装 DepthAI/OpenCV"));
#endif
}

void OakCameraVideoSource::stop()
{
    if (m_recording) {
        stopRecording();
    }
    m_active = false;
    emit stateChanged(State::Stopped, tr("OAK 相机已停止"));
}

bool OakCameraVideoSource::isActive() const
{
    return m_active;
}

bool OakCameraVideoSource::isConfigured() const
{
    return m_config.backend == QStringLiteral("oak_depthai");
}

bool OakCameraVideoSource::startRecording(const QString &directory, QString *outPath)
{
    Q_UNUSED(directory);
    if (outPath) {
        outPath->clear();
    }
    emit stateChanged(State::Error, tr("OAK 录像功能尚未完成实现"));
    return false;
}

QString OakCameraVideoSource::stopRecording()
{
    m_recording = false;
    const QString path = m_recordFilePath;
    m_recordFilePath.clear();
    return path;
}

bool OakCameraVideoSource::isRecording() const
{
    return m_recording;
}

bool OakCameraVideoSource::saveSnapshot(const QString &directory, QString *outPath) const
{
    Q_UNUSED(directory);
    if (outPath) {
        outPath->clear();
    }
    return false;
}

QImage OakCameraVideoSource::lastFrame() const
{
    return m_lastFrame;
}
