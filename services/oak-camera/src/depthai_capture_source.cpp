#include "oak_capture_source.h"
#include "oak_camera_device.h"

#include <depthai/depthai.hpp>
#include <stdexcept>

namespace {
class DepthAiCaptureSource final : public OakCaptureSource {
public:
    void open(const OakCameraConfig& config) override {
        dai::Pipeline pipeline;
        auto camera = pipeline.create<dai::node::ColorCamera>();
        camera->setBoardSocket(dai::CameraBoardSocket::CAM_A);
        camera->setResolution(dai::ColorCameraProperties::SensorResolution::THE_1080_P);
        camera->setPreviewSize(config.previewWidth, config.previewHeight);
        camera->setInterleaved(true);
        camera->setColorOrder(dai::ColorCameraProperties::ColorOrder::BGR);
        camera->setFps(config.previewFps);
        auto output = pipeline.create<dai::node::XLinkOut>();
        output->setStreamName("preview");
        camera->preview.link(output->input);
        device_ = std::make_unique<dai::Device>(pipeline);
        queue_ = device_->getOutputQueue("preview", 4, false);
    }

    cv::Mat read(std::chrono::milliseconds timeout) override {
        bool timedOut = false;
        const auto frame = queue_->get<dai::ImgFrame>(timeout, timedOut);
        if(timedOut || !frame) return {};
        const auto width = frame->getWidth();
        const auto height = frame->getHeight();
        auto& data = frame->getData();
        if(width == 0 || height == 0 || width > 4096 || height > 4096 ||
           data.size() < static_cast<std::size_t>(width) * height * 3) {
            throw std::runtime_error("invalid preview frame dimensions or payload");
        }
        // The ImgFrame owns data; return an independent image before it expires.
        return cv::Mat(static_cast<int>(height), static_cast<int>(width), CV_8UC3, data.data()).clone();
    }

    std::string deviceId() const override { return device_->getMxId(); }

private:
    // Queue is destroyed before the device.
    std::unique_ptr<dai::Device> device_;
    std::shared_ptr<dai::DataOutputQueue> queue_;
};
}

std::unique_ptr<OakCaptureSource> makeDepthAiCaptureSource() {
    return std::make_unique<DepthAiCaptureSource>();
}
