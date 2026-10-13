#include "oak_native_recorder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <cstring>

namespace {
void check(int code, const char* operation) {
    if(code >= 0) return;
    char message[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(code, message, sizeof(message));
    throw std::runtime_error(std::string(operation) + ": " + message);
}

struct EncodingSession {
    AVFormatContext* format = nullptr;
    AVCodecContext* encoder = nullptr;
    AVFrame* converted = av_frame_alloc();
    AVPacket* output = av_packet_alloc();
    AVStream* stream = nullptr;
    bool header = false;
    ~EncodingSession() {
        av_packet_free(&output);
        av_frame_free(&converted);
        avcodec_free_context(&encoder);
        if(format) {
            if(format->pb) avio_closep(&format->pb);
            avformat_free_context(format);
        }
    }
    void open(const std::filesystem::path& path, int w, int h, float fps, int crf) {
        if(!output || !converted) throw std::bad_alloc();
        const auto* codec = avcodec_find_encoder_by_name("libx264");
        if(!codec) throw std::runtime_error("native MP4 requires libx264 encoder");
        encoder = avcodec_alloc_context3(codec);
        if(!encoder) throw std::bad_alloc();
        encoder->width = w; encoder->height = h;
        encoder->pix_fmt = AV_PIX_FMT_YUV420P;
        encoder->time_base = {1, 1000};
        encoder->framerate = av_d2q(fps, 1000);
        encoder->gop_size = std::max(1, static_cast<int>(fps * 2));
        encoder->max_b_frames = 0;
        encoder->thread_count = 4;
        encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        check(av_opt_set(encoder->priv_data, "preset", "superfast", 0), "set H264 preset");
        check(av_opt_set(encoder->priv_data, "tune", "zerolatency", 0), "set H264 latency");
        check(av_opt_set_int(encoder->priv_data, "crf", crf, 0), "set H264 quality");
        check(avcodec_open2(encoder, codec, nullptr), "open H264 encoder");
        check(avformat_alloc_output_context2(&format, nullptr, "mp4", path.c_str()), "allocate MP4");
        if(!format) throw std::runtime_error("allocate MP4 failed");
        stream = avformat_new_stream(format, nullptr);
        if(!stream) throw std::bad_alloc();
        stream->time_base = encoder->time_base;
        stream->avg_frame_rate = encoder->framerate;
        check(avcodec_parameters_from_context(stream->codecpar, encoder), "set MP4 codec");
        check(avio_open(&format->pb, path.c_str(), AVIO_FLAG_WRITE), "open MP4 file");
        AVDictionary* options = nullptr;
        av_dict_set(&options, "movflags", "frag_keyframe+empty_moov+default_base_moof", 0);
        av_dict_set(&options, "frag_duration", "2000000", 0);
        const auto result = avformat_write_header(format, &options);
        av_dict_free(&options);
        check(result, "write MP4 header"); header = true;
        converted->format = encoder->pix_fmt;
        converted->width = w; converted->height = h;
        check(av_frame_get_buffer(converted, 32), "allocate native YUV frame");
    }
    void packets() {
        for(;;) {
            const auto result = avcodec_receive_packet(encoder, output);
            if(result == AVERROR(EAGAIN) || result == AVERROR_EOF) return;
            check(result, "receive H264 packet");
            av_packet_rescale_ts(output, encoder->time_base, stream->time_base);
            output->stream_index = stream->index;
            if(output->duration == 0) output->duration = av_rescale_q(1, av_inv_q(encoder->framerate), stream->time_base);
            const auto written = av_interleaved_write_frame(format, output);
            av_packet_unref(output);
            check(written, "write MP4 packet");
            if(format->pb->error) check(format->pb->error, "MP4 disk write");
        }
    }
    void write(const std::vector<std::uint8_t>& yuv, std::int64_t pts) {
        const auto w = encoder->width;
        const auto h = encoder->height;
        if(yuv.size() != static_cast<std::size_t>(w) * h * 3 / 2) {
            throw std::runtime_error("native YUV dimensions or payload changed during recording");
        }
        check(av_frame_make_writable(converted), "prepare native YUV frame");
        std::size_t offset = 0;
        for(int plane = 0; plane < 3; ++plane) {
            const auto width = plane == 0 ? w : w / 2;
            const auto height = plane == 0 ? h : h / 2;
            for(int row = 0; row < height; ++row) {
                std::memcpy(converted->data[plane] + row * converted->linesize[plane], yuv.data() + offset, width);
                offset += width;
            }
        }
        converted->pts = pts;
        check(avcodec_send_frame(encoder, converted), "encode native H264 frame");
        packets();
    }
    void finish() {
        if(!header) return;
        check(avcodec_send_frame(encoder, nullptr), "flush H264 encoder"); packets();
        check(av_write_trailer(format), "finalize MP4");
        avio_flush(format->pb);
        if(format->pb->error) check(format->pb->error, "final MP4 disk write");
        check(avio_closep(&format->pb), "close MP4 file");
        header = false;
    }
};
}

struct OakNativeRecorder::Impl {
    struct Item { std::shared_ptr<const std::vector<std::uint8_t>> yuv; std::chrono::steady_clock::time_point timestamp; };
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::deque<Item> queue;
    std::thread thread;
    bool stopping = false;
    bool ready = false;
    OakRecordingStats stats;

    void run(std::filesystem::path path, int w, int h, float fps, int crf) {
        EncodingSession session;
        std::string error;
        try {
            session.open(path, w, h, fps, crf);
            { std::lock_guard<std::mutex> lock(mutex); stats.active = true; ready = true; cv.notify_all(); }
            std::chrono::steady_clock::time_point first{};
            std::int64_t previous = -1;
            for(;;) {
                Item item;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    cv.wait(lock, [&] { return stopping || !queue.empty(); });
                    if(queue.empty()) break;
                    item = std::move(queue.front()); queue.pop_front();
                }
                if(first == std::chrono::steady_clock::time_point{}) first = item.timestamp;
                const auto pts = std::max(previous + 1,
                    std::chrono::duration_cast<std::chrono::milliseconds>(item.timestamp - first).count());
                session.write(*item.yuv, pts); previous = pts;
                { std::lock_guard<std::mutex> lock(mutex); ++stats.frames; }
            }
        } catch(const std::exception& e) { error = e.what(); }
        try { session.finish(); } catch(const std::exception& e) { if(error.empty()) error = e.what(); }
        {
            std::lock_guard<std::mutex> lock(mutex);
            stats.active = false; stats.error = error; ready = true;
            if(stats.error.empty() && stats.frames == 0) stats.error = "recording contains no frames";
            stats.dropped += queue.size(); queue.clear(); cv.notify_all();
        }
    }
};

OakNativeRecorder::OakNativeRecorder() : impl_(std::make_unique<Impl>()) {}
OakNativeRecorder::~OakNativeRecorder() { stop(); }
void OakNativeRecorder::start(const std::filesystem::path& path, int w, int h, float fps, int crf) {
    stop();
    std::unique_lock<std::mutex> lock(impl_->mutex);
    impl_->stats = {}; impl_->stopping = false; impl_->ready = false;
    impl_->thread = std::thread(&Impl::run, impl_.get(), path, w, h, fps, crf);
    impl_->cv.wait(lock, [&] { return impl_->ready; });
    if(!impl_->stats.error.empty()) throw std::runtime_error(impl_->stats.error);
}
void OakNativeRecorder::submit(std::shared_ptr<const std::vector<std::uint8_t>> yuv, std::chrono::steady_clock::time_point timestamp) {
    if(!yuv) throw std::invalid_argument("native recording frame is null");
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if(!impl_->stats.active || impl_->stopping) return;
    if(impl_->queue.size() >= 3) { impl_->queue.pop_front(); ++impl_->stats.dropped; }
    impl_->queue.push_back({std::move(yuv), timestamp}); impl_->cv.notify_one();
}
OakRecordingStats OakNativeRecorder::stop() {
    { std::lock_guard<std::mutex> lock(impl_->mutex); impl_->stopping = true; impl_->cv.notify_all(); }
    if(impl_->thread.joinable()) impl_->thread.join();
    return stats();
}
OakRecordingStats OakNativeRecorder::stats() const {
    std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->stats;
}
