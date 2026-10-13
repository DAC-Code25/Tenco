#include "oak_camera_device.h"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <unistd.h>

namespace {
void require(bool value, const char* message) {
    if(!value) throw std::runtime_error(message);
}
template<typename F> void waitFor(F predicate, const char* message) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while(!predicate()) {
        if(std::chrono::steady_clock::now() > until) throw std::runtime_error(message);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
struct Control {
    std::atomic<int> mode{0}; // healthy, throw, stall, missing
    std::atomic<int> opens{0};
};
class ControlledSource : public OakCaptureSource {
public:
    explicit ControlledSource(Control& c) : control(c) {}
    void open(const OakCameraConfig&) override {
        ++control.opens;
        if(control.mode == 3) throw std::runtime_error("missing test device");
    }
    OakCaptureFrame read(std::chrono::milliseconds) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if(control.mode == 1 || control.mode == 3) throw std::runtime_error("test X_LINK_ERROR");
        if(control.mode == 2) return {};
        cv::Mat yuv;
        cv::cvtColor(cv::Mat(64, 96, CV_8UC3, cv::Scalar(0, 150, 0)), yuv, cv::COLOR_BGR2YUV_I420);
        return {cv::Mat(32, 48, CV_8UC3, cv::Scalar(0, 150, 0)).clone(), std::make_shared<const std::vector<std::uint8_t>>(yuv.data, yuv.data + yuv.total()), std::chrono::steady_clock::now()};
    }
    OakCaptureInfo info() const override { return {"test-native", 96, 64}; }
    std::string deviceId() const override { return "controlled-test"; }
    Control& control;
};
}

int main() {
    const auto directory = std::filesystem::temp_directory_path() /
        ("oak-recovery-test-" + std::to_string(getpid()));
    try {
        OakCameraConfig config;
        config.snapshotDir = directory / "photos";
        config.recordingDir = directory / "records";
        config.frameFreshnessMs = 80;
        config.noFrameTimeoutMs = 160;
        config.reconnectIntervalMs = 20;
        config.reconnectMaxIntervalMs = 80;
        Control control;
        OakCameraDevice camera(config, [&] { return std::make_unique<ControlledSource>(control); });
        camera.start();
        waitFor([&] { return camera.status().cameraConnected; }, "initial capture failed");
        std::uint64_t id = 0;
        std::vector<std::uint8_t> jpeg;
        require(camera.waitForNextJpeg(0, id, jpeg, std::chrono::milliseconds(0)), "initial JPEG missing");
        require(!jpeg.empty() && id > 0, "JPEG/frame ID invalid");
        require(camera.startRecording().success, "recording did not start");
        control.mode = 1;
        waitFor([&] { return camera.status().reconnectCount > 0; }, "fault did not trigger retry");
        require(!camera.latestJpeg() && !camera.status().recording, "failed stream retained frame or recording");
        require(!camera.waitForNextJpeg(0, id, jpeg, std::chrono::milliseconds(5)), "failed stream returned cache");
        const auto before = camera.status().frameId;
        control.mode = 0;
        waitFor([&] { return camera.status().cameraConnected; }, "capture did not reconnect");
        require(camera.status().frameId > before, "frame ID did not increase across retry");
        require(!camera.status().recording, "recording resumed without command");
        require(camera.status().lastCaptureError == "test X_LINK_ERROR", "failure diagnostics lost");
        const auto retries = camera.status().reconnectCount;
        control.mode = 2;
        waitFor([&] { return !camera.status().cameraConnected; }, "stale frame reported healthy");
        require(!camera.latestJpeg(), "stale cache exposed");
        waitFor([&] { return camera.status().reconnectCount > retries; }, "stall did not rebuild source");
        require(camera.status().lastCaptureError.find("stalled") != std::string::npos, "stall error missing");
        control.mode = 0;
        waitFor([&] { return camera.status().cameraConnected; }, "stall did not recover");
        camera.stop(); camera.stop();
        require(!camera.latestJpeg() && !camera.status().cameraConnected, "stop retained healthy frame");
        camera.start();
        waitFor([&] { return camera.status().cameraConnected; }, "same object restart failed");
        camera.stop();
        control.mode = 3;
        config.reconnectIntervalMs = 2000;
        config.reconnectMaxIntervalMs = 2000;
        OakCameraDevice missing(config, [&] { return std::make_unique<ControlledSource>(control); });
        missing.start();
        waitFor([&] { return missing.status().reconnectCount > 0; }, "initial missing device not reported");
        const auto stopStart = std::chrono::steady_clock::now();
        missing.stop();
        require(std::chrono::steady_clock::now() - stopStart < std::chrono::milliseconds(300), "stop failed to interrupt backoff");
        auto invalid = config; invalid.previewFps = std::numeric_limits<float>::quiet_NaN();
        bool rejected = false;
        try { OakCameraDevice bad(invalid); } catch(const std::invalid_argument&) { rejected = true; }
        require(rejected, "nonfinite configuration accepted");
        std::filesystem::remove_all(directory);
        std::cout << "capture failure, missing source, stall, cache, recording, retry, restart and stop passed\n";
        return 0;
    } catch(const std::exception& e) {
        std::cerr << e.what() << '\n';
        std::filesystem::remove_all(directory);
        return 1;
    }
}
