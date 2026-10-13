#include "oak_capture_source.h"
#include "oak_camera_device.h"

#include <depthai/depthai.hpp>
#include <stdexcept>

namespace {
class DepthAiCaptureSource final : public OakCaptureSource {
public:
    void open(const OakCameraConfig& config) override {
        device_ = std::make_unique<dai::Device>();
        for(const auto& feature : device_->getConnectedCameraFeatures()) {
            if(feature.socket == dai::CameraBoardSocket::CAM_A) {
                info_ = {feature.sensorName, feature.width, feature.height};
            }
        }
        dai::Pipeline pipeline;
        auto camera = pipeline.create<dai::node::ColorCamera>();
        camera->setBoardSocket(dai::CameraBoardSocket::CAM_A);
        // Refuse an unrecognized native mode instead of silently using a smaller image.
        if(info_.nativeWidth == 4056 && info_.nativeHeight == 3040) {
            camera->setResolution(dai::ColorCameraProperties::SensorResolution::THE_12_MP);
        } else if(info_.nativeWidth == 4208 && info_.nativeHeight == 3120) {
            camera->setResolution(dai::ColorCameraProperties::SensorResolution::THE_13_MP);
        } else {
            throw std::runtime_error("unsupported native color sensor mode: " + info_.sensorName);
        }
        camera->setIspScale(1, 1);
        camera->setPreviewSize(config.previewWidth, config.previewHeight);
        camera->setInterleaved(true);
        camera->setColorOrder(dai::ColorCameraProperties::ColorOrder::BGR);
        camera->setFps(config.previewFps);
        auto output = pipeline.create<dai::node::XLinkOut>();
        output->setStreamName("preview");
        camera->preview.link(output->input);
        auto nativeOutput = pipeline.create<dai::node::XLinkOut>();
        nativeOutput->setStreamName("native");
        nativeOutput->input.setQueueSize(2);
        nativeOutput->input.setBlocking(false);
        // Native width 4056 is not divisible by the device encoder's required 32.
        // Transport ISP YUV420 directly over USB3 without cropping or resizing.
        camera->isp.link(nativeOutput->input);
        device_->startPipeline(pipeline);
        queue_ = device_->getOutputQueue("preview", 2, false);
        nativeQueue_ = device_->getOutputQueue("native", 2, false);
    }

    OakCaptureFrame read(std::chrono::milliseconds timeout) override {
        OakCaptureFrame result;
        bool timedOut = false;
        const auto frame = queue_->get<dai::ImgFrame>(timeout, timedOut);
        if(auto native = nativeQueue_->tryGet<dai::ImgFrame>()) {
            if(static_cast<int>(native->getWidth()) != info_.nativeWidth ||
               static_cast<int>(native->getHeight()) != info_.nativeHeight ||
               native->getType() != dai::ImgFrame::Type::YUV420p ||
               native->getData().size() != static_cast<std::size_t>(info_.nativeWidth) * info_.nativeHeight * 3 / 2) {
                throw std::runtime_error("invalid native ISP YUV420 dimensions or payload");
            }
            result.nativeYuv = std::make_shared<const std::vector<std::uint8_t>>(std::move(native->getData()));
            result.capturedAt = native->getTimestamp();
        }
        if(timedOut || !frame) return result;
        const auto width = frame->getWidth();
        const auto height = frame->getHeight();
        auto& data = frame->getData();
        if(width == 0 || height == 0 || width > 4096 || height > 4096 ||
           data.size() < static_cast<std::size_t>(width) * height * 3) {
            throw std::runtime_error("invalid preview frame dimensions or payload");
        }
        // The ImgFrame owns data; return an independent image before it expires.
        result.preview = cv::Mat(static_cast<int>(height), static_cast<int>(width), CV_8UC3, data.data()).clone();
        return result;
    }

    std::string deviceId() const override { return device_->getMxId(); }
    OakCaptureInfo info() const override { return info_; }

private:
    // Queue is destroyed before the device.
    std::unique_ptr<dai::Device> device_;
    std::shared_ptr<dai::DataOutputQueue> queue_;
    std::shared_ptr<dai::DataOutputQueue> nativeQueue_;
    OakCaptureInfo info_;
};
}

std::unique_ptr<OakCaptureSource> makeDepthAiCaptureSource() {
    return std::make_unique<DepthAiCaptureSource>();
}
