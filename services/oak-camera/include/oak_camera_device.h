#pragma once

#include "oak_capture_source.h"
#include "oak_native_recorder.h"
#include <opencv2/opencv.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

struct OakCameraConfig {
    std::string bindAddress = "0.0.0.0";
    int port = 18080;
    int previewWidth = 960;
    int previewHeight = 540;
    float previewFps = 15.0f;
    int jpegQuality = 70;
    int captureJpegQuality = 90;
    int recordingCrf = 23;
    bool lowLatencyProfile = true;
    int frameFreshnessMs = 1000;
    int noFrameTimeoutMs = 5000;
    int reconnectIntervalMs = 1000;
    int reconnectMaxIntervalMs = 10000;
    std::filesystem::path snapshotDir = "Capture";
    std::filesystem::path recordingDir = "Capture";
    std::string mjpegBoundary = "tenco-oak-boundary";
};

struct OakStatus {
    bool success = true;
    std::string message = "ok";
    bool cameraConnected = false;
    bool recording = false;
    std::string path;
    std::string deviceId;
    int previewWidth = 0;
    int previewHeight = 0;
    float previewFps = 0.0f;
    int jpegQuality = 0;
    std::string snapshotDir;
    std::string recordingDir;
    std::string lastPhotoPath;
    std::string lastRecordingPath;
    std::string lastCaptureError;
    std::uint64_t reconnectCount = 0;
    std::uint64_t frameId = 0;
    std::int64_t lastFrameAgeMs = -1;
    std::string sensorName;
    int nativeWidth = 0;
    int nativeHeight = 0;
    int captureJpegQuality = 0;
    int recordingCrf = 0;
    std::string recordingCodec = "h264";
    std::string recordingContainer = "mp4";
    std::int64_t lastNativeFrameAgeMs = -1;
    std::uint64_t nativeFrameId = 0;
    std::uint64_t recordingFrames = 0;
    std::uint64_t recordingDroppedFrames = 0;
    std::string lastRecordingError;
};

class OakCameraDevice {
  public:
    explicit OakCameraDevice(OakCameraConfig config, OakCaptureFactory factory = makeDepthAiCaptureSource);
    ~OakCameraDevice();

    OakCameraDevice(const OakCameraDevice&) = delete;
    OakCameraDevice& operator=(const OakCameraDevice&) = delete;

    void start();
    void stop();

    OakStatus status() const;
    OakStatus savePhoto();
    OakStatus startRecording();
    OakStatus stopRecording();

    bool waitForNextJpeg(std::uint64_t afterFrameId, std::uint64_t& outFrameId, std::vector<std::uint8_t>& outJpeg, std::chrono::milliseconds timeout) const;
    std::optional<std::vector<std::uint8_t>> latestJpeg() const;

  private:
    void runCaptureLoop();
    static std::string makeTimestamp();
    std::vector<std::uint8_t> encodeJpeg(const cv::Mat& frame) const;
    OakStatus currentStatusLocked() const;
    bool hasFreshFrameLocked() const;
    bool hasFreshNativeFrameLocked() const;
    void clearFramesLocked();

    OakCameraConfig config_;
    OakCaptureFactory factory_;
    std::mutex lifecycleMutex_;

    mutable std::mutex mutex_;
    mutable std::mutex controlMutex_;
    mutable std::condition_variable frameCv_;

    std::atomic<bool> running_{false};
    std::thread worker_;

    bool cameraConnected_ = false;
    bool recording_ = false;
    std::string lastMessage_ = "not started";
    std::string deviceId_;
    std::string lastPhotoPath_;
    std::string lastRecordingPath_;
    std::uint64_t frameId_ = 0;
    std::uint64_t reconnectCount_ = 0;
    std::string lastCaptureError_;
    std::chrono::steady_clock::time_point lastFrameTime_{};

    std::vector<std::uint8_t> latestJpeg_;
    std::shared_ptr<const std::vector<std::uint8_t>> latestNativeYuv_;
    std::chrono::steady_clock::time_point lastNativeFrameTime_{};
    std::uint64_t nativeFrameId_ = 0;
    OakCaptureInfo captureInfo_;
    OakNativeRecorder writer_;
};

std::string oakStatusToJson(const OakStatus& status);
std::string oakEscapeJson(const std::string& text);
void validateOakCameraConfig(const OakCameraConfig& config);
