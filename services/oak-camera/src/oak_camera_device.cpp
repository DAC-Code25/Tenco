#include "oak_camera_device.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <cmath>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {

std::string pathOrEmpty(const std::string& path) {
    return path;
}

}  // namespace

OakCameraDevice::OakCameraDevice(OakCameraConfig config, OakCaptureFactory factory)
    : config_(std::move(config)), factory_(std::move(factory)) {
    validateOakCameraConfig(config_);
    if(!factory_) throw std::invalid_argument("capture factory is empty");
}

OakCameraDevice::~OakCameraDevice() {
    stop();
}

void OakCameraDevice::start() {
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
    if(running_.load()) return;
    std::filesystem::create_directories(config_.snapshotDir);
    std::filesystem::create_directories(config_.recordingDir);
    running_.store(true);
    try {
        worker_ = std::thread(&OakCameraDevice::runCaptureLoop, this);
    } catch(...) {
        running_.store(false);
        throw;
    }
}

void OakCameraDevice::stop() {
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
    running_.store(false);
    frameCv_.notify_all();
    if(worker_.joinable()) {
        worker_.join();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if(writer_.isOpened()) {
        writer_.release();
    }
    recording_ = false;
    cameraConnected_ = false;
    clearFramesLocked();
    deviceId_.clear();
    lastMessage_ = "stopped";
    frameCv_.notify_all();
}

OakStatus OakCameraDevice::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return currentStatusLocked();
}

OakStatus OakCameraDevice::savePhoto() {
    std::lock_guard<std::mutex> controlLock(controlMutex_);
    cv::Mat frame;
    std::filesystem::path outputPath;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!hasFreshFrameLocked()) {
            return currentStatusLocked();
        }
        if(latestFrame_.empty()) {
            OakStatus result = currentStatusLocked();
            result.success = false;
            result.message = "no preview frame available";
            return result;
        }
        frame = latestFrame_.clone();
        outputPath = config_.snapshotDir / ("oak_photo_" + makeTimestamp() + ".jpg");
    }

    if(!cv::imwrite(outputPath.string(), frame)) {
        std::lock_guard<std::mutex> lock(mutex_);
        OakStatus result = currentStatusLocked();
        result.success = false;
        result.message = "failed to save photo";
        return result;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    lastMessage_ = "photo saved";
    lastPhotoPath_ = outputPath.string();
    OakStatus result = currentStatusLocked();
    result.message = "photo saved";
    result.path = outputPath.string();
    return result;
}

OakStatus OakCameraDevice::startRecording() {
    std::lock_guard<std::mutex> controlLock(controlMutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    if(!hasFreshFrameLocked()) {
        OakStatus result = currentStatusLocked();
        result.success = false;
        result.message = "camera not connected";
        return result;
    }
    if(latestFrame_.empty()) {
        OakStatus result = currentStatusLocked();
        result.success = false;
        result.message = "no preview frame available";
        return result;
    }
    if(recording_) {
        OakStatus result = currentStatusLocked();
        result.message = "recording already started";
        result.path = lastRecordingPath_;
        return result;
    }

    const auto outputPath = config_.recordingDir / ("oak_record_" + makeTimestamp() + ".avi");
    writer_.open(outputPath.string(),
                 cv::VideoWriter::fourcc('M', 'J', 'P', 'G'),
                 config_.previewFps,
                 latestFrame_.size(),
                 latestFrame_.channels() == 3);
    if(!writer_.isOpened()) {
        OakStatus result = currentStatusLocked();
        result.success = false;
        result.message = "failed to open recording file";
        return result;
    }

    recording_ = true;
    lastRecordingPath_ = outputPath.string();
    lastMessage_ = "recording started";

    OakStatus result = currentStatusLocked();
    result.message = "recording started";
    result.path = lastRecordingPath_;
    return result;
}

OakStatus OakCameraDevice::stopRecording() {
    std::lock_guard<std::mutex> controlLock(controlMutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    if(!recording_) {
        OakStatus result = currentStatusLocked();
        result.message = "recording already stopped";
        result.path = lastRecordingPath_;
        result.recording = false;
        return result;
    }

    writer_.release();
    recording_ = false;
    lastMessage_ = "recording stopped";

    OakStatus result = currentStatusLocked();
    result.message = "recording stopped";
    result.path = lastRecordingPath_;
    result.recording = false;
    return result;
}

bool OakCameraDevice::waitForNextJpeg(std::uint64_t afterFrameId, std::uint64_t& outFrameId, std::vector<std::uint8_t>& outJpeg, std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(mutex_);
    if(!running_.load() || !hasFreshFrameLocked()) return false;
    const auto ready = frameCv_.wait_for(lock, timeout, [&] {
        return !running_.load() || !cameraConnected_ || frameId_ > afterFrameId;
    });

    if(!ready || !running_.load() || !hasFreshFrameLocked() || frameId_ <= afterFrameId) {
        return false;
    }

    outFrameId = frameId_;
    outJpeg = latestJpeg_;
    return true;
}

std::optional<std::vector<std::uint8_t>> OakCameraDevice::latestJpeg() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if(!hasFreshFrameLocked()) {
        return std::nullopt;
    }
    return latestJpeg_;
}

void OakCameraDevice::runCaptureLoop() {
    int retryDelayMs = config_.reconnectIntervalMs;
    while(running_.load()) {
        std::unique_ptr<OakCaptureSource> source;
        std::string failure;
        try {
            source = factory_();
            if(!source) throw std::runtime_error("capture factory returned no source");
            source->open(config_);
            const std::string id = source->deviceId();
            auto lastValidFrame = std::chrono::steady_clock::now();
            auto healthySince = lastValidFrame;
            bool seenFrame = false;
            while(running_.load()) {
                cv::Mat image = source->read(std::chrono::milliseconds(100));
                const auto now = std::chrono::steady_clock::now();
                if(now - lastValidFrame >= std::chrono::milliseconds(config_.noFrameTimeoutMs)) {
                    throw std::runtime_error("preview stalled: no valid new frame within timeout");
                }
                if(image.empty()) continue;
                if(image.type() != CV_8UC3 || image.cols < 1 || image.rows < 1) {
                    throw std::runtime_error("invalid BGR preview image");
                }
                auto jpeg = encodeJpeg(image);
                if(jpeg.empty()) throw std::runtime_error("JPEG encoding produced no frame");
                lastValidFrame = now;
                {
                    std::lock_guard<std::mutex> controlLock(controlMutex_);
                    std::lock_guard<std::mutex> lock(mutex_);
                    if(!running_.load()) break;
                    latestFrame_ = image.clone();
                    latestJpeg_ = std::move(jpeg);
                    lastFrameTime_ = now;
                    ++frameId_;
                    cameraConnected_ = true;
                    deviceId_ = id;
                    if(!seenFrame) lastMessage_ = "camera connected";
                    if(recording_ && writer_.isOpened()) writer_.write(latestFrame_);
                    frameCv_.notify_all();
                }
                if(!seenFrame) {
                    std::clog << makeTimestamp() << " capture connected: " << id << std::endl;
                    healthySince = now;
                    seenFrame = true;
                }
                // Short lived connections do not keep resetting the backoff.
                if(now - healthySince >= std::chrono::milliseconds(config_.noFrameTimeoutMs)) {
                    retryDelayMs = config_.reconnectIntervalMs;
                }
            }
        } catch(const std::exception& e) {
            failure = e.what();
        } catch(...) {
            failure = "unknown capture exception";
        }
        {
            std::lock_guard<std::mutex> controlLock(controlMutex_);
            std::lock_guard<std::mutex> lock(mutex_);
            cameraConnected_ = false;
            deviceId_.clear();
            clearFramesLocked();
            recording_ = false;
            if(writer_.isOpened()) writer_.release();
            if(!failure.empty() && running_.load()) {
                lastCaptureError_ = failure;
                lastMessage_ = "capture loop error: " + failure;
                ++reconnectCount_;
            }
            frameCv_.notify_all();
        }
        if(running_.load()) {
            // Log before SDK cleanup so a blocking destructor cannot hide the failure.
            std::cerr << makeTimestamp() << " capture failed: " << failure
                      << "; retry in " << retryDelayMs << " ms" << std::endl;
        }
        // Device cleanup may block in the SDK. Never hold HTTP state locks here.
        source.reset();
        if(!running_.load()) break;
        std::unique_lock<std::mutex> lock(mutex_);
        frameCv_.wait_for(lock, std::chrono::milliseconds(retryDelayMs), [this] { return !running_.load(); });
        retryDelayMs = std::min(config_.reconnectMaxIntervalMs, retryDelayMs * 2);
    }
}

bool OakCameraDevice::hasFreshFrameLocked() const {
    return running_.load() && cameraConnected_ && !latestJpeg_.empty() &&
        std::chrono::steady_clock::now() - lastFrameTime_ <= std::chrono::milliseconds(config_.frameFreshnessMs);
}

void OakCameraDevice::clearFramesLocked() {
    latestFrame_.release();
    latestJpeg_.clear();
    lastFrameTime_ = {};
}

std::string OakCameraDevice::makeTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm localTime{};
    localtime_r(&time, &localTime);

    std::ostringstream oss;
    oss << std::put_time(&localTime, "%Y%m%d_%H%M%S");
    return oss.str();
}

std::vector<std::uint8_t> OakCameraDevice::encodeJpeg(const cv::Mat& frame) const {
    std::vector<std::uint8_t> encoded;
    if(!cv::imencode(".jpg", frame, encoded, {cv::IMWRITE_JPEG_QUALITY, config_.jpegQuality})) {
        return {};
    }
    return encoded;
}

OakStatus OakCameraDevice::currentStatusLocked() const {
    OakStatus status;
    status.success = hasFreshFrameLocked();
    status.message = lastMessage_;
    status.cameraConnected = status.success;
    if(cameraConnected_ && !status.success) status.message = "preview frame stale";
    status.recording = recording_;
    status.path = lastRecordingPath_;
    status.deviceId = deviceId_;
    status.previewWidth = config_.previewWidth;
    status.previewHeight = config_.previewHeight;
    status.previewFps = config_.previewFps;
    status.jpegQuality = config_.jpegQuality;
    status.snapshotDir = config_.snapshotDir.string();
    status.recordingDir = config_.recordingDir.string();
    status.lastPhotoPath = lastPhotoPath_;
    status.lastRecordingPath = lastRecordingPath_;
    status.lastCaptureError = lastCaptureError_;
    status.reconnectCount = reconnectCount_;
    status.frameId = frameId_;
    if(lastFrameTime_ != std::chrono::steady_clock::time_point{}) {
        status.lastFrameAgeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - lastFrameTime_).count();
    }
    return status;
}

std::string oakEscapeJson(const std::string& text) {
    std::ostringstream oss;
    for(char ch : text) {
        switch(ch) {
            case '\\':
                oss << "\\\\";
                break;
            case '"':
                oss << "\\\"";
                break;
            case '\n':
                oss << "\\n";
                break;
            case '\r':
                oss << "\\r";
                break;
            case '\t':
                oss << "\\t";
                break;
            default:
                oss << ch;
                break;
        }
    }
    return oss.str();
}

std::string oakStatusToJson(const OakStatus& status) {
    std::ostringstream oss;
    oss << "{"
        << "\"success\":" << (status.success ? "true" : "false") << ","
        << "\"message\":\"" << oakEscapeJson(status.message) << "\","
        << "\"path\":\"" << oakEscapeJson(pathOrEmpty(status.path)) << "\","
        << "\"deviceId\":\"" << oakEscapeJson(status.deviceId) << "\","
        << "\"cameraConnected\":" << (status.cameraConnected ? "true" : "false") << ","
        << "\"recording\":" << (status.recording ? "true" : "false") << ","
        << "\"previewWidth\":" << status.previewWidth << ","
        << "\"previewHeight\":" << status.previewHeight << ","
        << "\"previewFps\":" << status.previewFps << ","
        << "\"jpegQuality\":" << status.jpegQuality << ","
        << "\"snapshotDir\":\"" << oakEscapeJson(status.snapshotDir) << "\","
        << "\"recordingDir\":\"" << oakEscapeJson(status.recordingDir) << "\","
        << "\"lastPhotoPath\":\"" << oakEscapeJson(status.lastPhotoPath) << "\","
        << "\"lastRecordingPath\":\"" << oakEscapeJson(status.lastRecordingPath) << "\","
        << "\"lastCaptureError\":\"" << oakEscapeJson(status.lastCaptureError) << "\","
        << "\"reconnectCount\":" << status.reconnectCount << ","
        << "\"frameId\":" << status.frameId << ","
        << "\"lastFrameAgeMs\":" << status.lastFrameAgeMs
        << "}";
    return oss.str();
}

void validateOakCameraConfig(const OakCameraConfig& c) {
    if(c.port < 1 || c.port > 65535 || c.previewWidth < 16 || c.previewWidth > 4096 ||
       c.previewHeight < 16 || c.previewHeight > 4096 || !std::isfinite(c.previewFps) ||
       c.previewFps < 1 || c.previewFps > 60 || c.jpegQuality < 1 || c.jpegQuality > 100 ||
       c.frameFreshnessMs < 50 || c.frameFreshnessMs > 60000 ||
       c.noFrameTimeoutMs < c.frameFreshnessMs || c.noFrameTimeoutMs > 60000 ||
       c.reconnectIntervalMs < 1 || c.reconnectMaxIntervalMs < c.reconnectIntervalMs ||
       c.reconnectMaxIntervalMs > 60000 || c.snapshotDir.empty() || c.recordingDir.empty()) {
        throw std::invalid_argument("invalid camera dimensions, FPS, quality, timeout, retry, port or media directory");
    }
}
