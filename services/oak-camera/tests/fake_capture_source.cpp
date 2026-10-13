#include "oak_camera_device.h"
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <thread>

namespace {
std::string mode() {
    const auto path = std::getenv("OAK_TEST_STATE_FILE");
    if(!path) return "healthy";
    std::ifstream file(path);
    std::string value;
    file >> value;
    return value;
}
class FakeCapture final : public OakCaptureSource {
public:
    void open(const OakCameraConfig&) override {
        if(mode() == "missing") throw std::runtime_error("test device unavailable");
    }
    cv::Mat read(std::chrono::milliseconds timeout) override {
        std::this_thread::sleep_for(std::min(timeout, std::chrono::milliseconds(20)));
        const auto current = mode();
        if(current == "fault" || current == "missing") throw std::runtime_error("test X_LINK_ERROR");
        if(current == "stall") return {};
        cv::Mat image(32, 48, CV_8UC3);
        image.setTo(cv::Scalar(sequence_++ % 255, 80, 120));
        return image;
    }
    std::string deviceId() const override { return "test-device"; }
private:
    int sequence_ = 0;
};
}
std::unique_ptr<OakCaptureSource> makeDepthAiCaptureSource() {
    return std::make_unique<FakeCapture>();
}
