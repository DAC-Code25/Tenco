#ifndef OAKCAMERAVIDEOSOURCE_H
#define OAKCAMERAVIDEOSOURCE_H

#include "abstractvideosource.h"

#include "configmanager.h"

#include <memory>

class OakCameraVideoSource : public AbstractVideoSource
{
    Q_OBJECT

public:
    explicit OakCameraVideoSource(const ConfigManager::VideoConfig &config, QObject *parent = nullptr);
    ~OakCameraVideoSource() override;

    void start() override;
    void stop() override;
    bool isActive() const override;
    bool isConfigured() const override;

    bool startRecording(const QString &directory, QString *outPath = nullptr) override;
    QString stopRecording() override;
    bool isRecording() const override;

    bool saveSnapshot(const QString &directory, QString *outPath = nullptr) const override;
    QImage lastFrame() const override;

    void setReconnectIntervalMs(int intervalMs) override;
    void setAutoReconnect(bool enabled) override;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

#endif // OAKCAMERAVIDEOSOURCE_H
