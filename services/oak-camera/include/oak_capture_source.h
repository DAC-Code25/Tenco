#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <opencv2/core.hpp>

struct OakCameraConfig;

// A source belongs exclusively to the capture thread, including destruction.
class OakCaptureSource {
public:
    virtual ~OakCaptureSource() = default;
    virtual void open(const OakCameraConfig& config) = 0;
    virtual cv::Mat read(std::chrono::milliseconds timeout) = 0;
    virtual std::string deviceId() const = 0;
};

using OakCaptureFactory = std::function<std::unique_ptr<OakCaptureSource>()>;
std::unique_ptr<OakCaptureSource> makeDepthAiCaptureSource();
