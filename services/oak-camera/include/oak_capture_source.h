#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <opencv2/core.hpp>

struct OakCameraConfig;

struct OakCaptureFrame {
    cv::Mat preview;
    std::shared_ptr<const std::vector<std::uint8_t>> nativeYuv;
    std::chrono::steady_clock::time_point capturedAt{};
};

struct OakCaptureInfo {
    std::string sensorName;
    int nativeWidth = 0;
    int nativeHeight = 0;
};

// A source belongs exclusively to the capture thread, including destruction.
class OakCaptureSource {
public:
    virtual ~OakCaptureSource() = default;
    virtual void open(const OakCameraConfig& config) = 0;
    virtual OakCaptureFrame read(std::chrono::milliseconds timeout) = 0;
    virtual std::string deviceId() const = 0;
    virtual OakCaptureInfo info() const = 0;
};

using OakCaptureFactory = std::function<std::unique_ptr<OakCaptureSource>()>;
std::unique_ptr<OakCaptureSource> makeDepthAiCaptureSource();
