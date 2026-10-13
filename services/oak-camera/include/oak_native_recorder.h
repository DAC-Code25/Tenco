#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

struct OakRecordingStats {
    bool active = false;
    std::uint64_t frames = 0;
    std::uint64_t dropped = 0;
    std::string error;
};

// The worker exclusively owns codec and muxer state. submit never encodes or waits for disk.
class OakNativeRecorder {
public:
    OakNativeRecorder();
    ~OakNativeRecorder();
    void start(const std::filesystem::path& path, int width, int height, float fps, int crf);
    void submit(std::shared_ptr<const std::vector<std::uint8_t>> yuv, std::chrono::steady_clock::time_point timestamp);
    OakRecordingStats stop();
    OakRecordingStats stats() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
