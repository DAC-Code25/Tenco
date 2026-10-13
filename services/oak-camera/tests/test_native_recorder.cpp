#include "oak_native_recorder.h"
#include <opencv2/opencv.hpp>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <unistd.h>

void require(bool value, const char* message) {
    if(!value) throw std::runtime_error(message);
}

int main() {
    const auto dir = std::filesystem::temp_directory_path() / ("oak-native-test-" + std::to_string(getpid()));
    std::filesystem::create_directories(dir);
    try {
        OakNativeRecorder recorder;
        bool rejected = false;
        try { recorder.start(dir / "missing" / "bad.mp4", 96, 64, 15, 18); }
        catch(const std::exception&) { rejected = true; }
        require(rejected, "file open failure accepted");
        recorder.stop();
        cv::Mat yuv;
        cv::cvtColor(cv::Mat(64, 96, CV_8UC3, cv::Scalar(20, 100, 220)), yuv, cv::COLOR_BGR2YUV_I420);
        const auto native = std::make_shared<const std::vector<std::uint8_t>>(yuv.data, yuv.data + yuv.total());
        recorder.start(dir / "native.mp4", 96, 64, 15, 18);
        const auto start = std::chrono::steady_clock::now();
        for(int i = 0; i < 8; ++i) {
            recorder.submit(native, start + std::chrono::milliseconds(i * 200));
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        const auto stats = recorder.stop();
        require(stats.frames == 8 && stats.dropped == 0 && stats.error.empty() && !stats.active,
                "recording did not drain or report frames");
        require(std::filesystem::file_size(dir / "native.mp4") > 1000, "MP4 empty");
        recorder.start(dir / "changed.mp4", 96, 64, 15, 18);
        recorder.submit(std::make_shared<const std::vector<std::uint8_t>>(48 * 32 * 3 / 2), start);
        const auto changed = recorder.stop();
        require(!changed.error.empty() && !changed.active, "dimension change ignored");
        recorder.start(dir / "corrupt.mp4", 96, 64, 15, 18);
        recorder.submit(std::make_shared<const std::vector<std::uint8_t>>(4), start);
        require(!recorder.stop().error.empty(), "invalid native payload silently accepted");
        std::filesystem::remove_all(dir);
        std::cout << "native MP4 drain, counters, file failure, dimension change and invalid native payload passed\n";
        return 0;
    } catch(const std::exception& e) {
        std::cerr << e.what() << '\n';
        std::filesystem::remove_all(dir);
        return 1;
    }
}
