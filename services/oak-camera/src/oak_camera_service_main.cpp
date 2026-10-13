#include "oak_camera_device.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <algorithm>
#include <memory>
#include <mutex>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>
#include <chrono>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

struct ClientConnection {
    std::mutex mutex;
    int fd = -1;
    std::atomic<bool> done{false};
    std::thread worker;
};

std::atomic<bool> gRunning{true};

void signalHandler(int) {
    gRunning.store(false);
}

struct HttpRequest {
    std::string method;
    std::string path;
};

HttpRequest parseRequestLine(const std::string& requestText) {
    std::istringstream stream(requestText);
    HttpRequest request;
    std::string version;
    stream >> request.method >> request.path >> version;
    return request;
}

bool sendAll(int fd, const void* data, std::size_t size) {
    const auto* ptr = static_cast<const char*>(data);
    std::size_t sent = 0;
    while(sent < size) {
        const auto written = ::send(fd, ptr + sent, size - sent, MSG_NOSIGNAL);
        if(written < 0 && errno == EINTR) continue;
        if(written <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(written);
    }
    return true;
}

bool sendTextResponse(int fd, int statusCode, const std::string& statusText, const std::string& contentType, const std::string& body) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << statusCode << ' ' << statusText << "\r\n"
        << "Connection: close\r\n"
        << "Content-Type: " << contentType << "\r\n"
        << "Content-Length: " << body.size() << "\r\n\r\n"
        << body;
    const auto text = oss.str();
    return sendAll(fd, text.data(), text.size());
}

std::string makeJsonResponse(OakCameraDevice& camera, const OakStatus& status) {
    (void)camera;
    return oakStatusToJson(status);
}

void handleMjpegStream(int fd, OakCameraDevice& camera, const OakCameraConfig& config) {
    std::uint64_t lastFrameId = 0;
    std::vector<std::uint8_t> jpeg;
    if(!camera.waitForNextJpeg(0, lastFrameId, jpeg, std::chrono::milliseconds(0))) {
        sendTextResponse(fd, 503, "Service Unavailable", "application/json", oakStatusToJson(camera.status()));
        return;
    }
    std::ostringstream header;
    header << "HTTP/1.1 200 OK\r\n"
           << "Connection: close\r\n"
           << "Cache-Control: no-store, no-cache, must-revalidate, max-age=0\r\n"
           << "Pragma: no-cache\r\n"
           << "Content-Type: multipart/x-mixed-replace; boundary=" << config.mjpegBoundary << "\r\n\r\n";
    const auto headerText = header.str();
    if(!sendAll(fd, headerText.data(), headerText.size())) {
        return;
    }

    while(gRunning.load()) {
        std::ostringstream partHeader;
        partHeader << "--" << config.mjpegBoundary << "\r\n"
                   << "Content-Type: image/jpeg\r\n"
                   << "Content-Length: " << jpeg.size() << "\r\n\r\n";
        const auto partText = partHeader.str();
        if(!sendAll(fd, partText.data(), partText.size())) {
            break;
        }
        if(!sendAll(fd, jpeg.data(), jpeg.size())) {
            break;
        }
        if(!sendAll(fd, "\r\n", 2)) {
            break;
        }
        std::uint64_t currentFrameId = lastFrameId;
        jpeg.clear();
        while(gRunning.load() && jpeg.empty() && camera.status().cameraConnected) {
            camera.waitForNextJpeg(lastFrameId, currentFrameId, jpeg,
                                  std::chrono::milliseconds(config.frameFreshnessMs));
        }
        if(jpeg.empty()) break;
        lastFrameId = currentFrameId;
    }
}

void handleClient(int fd, OakCameraDevice& camera, const OakCameraConfig& config) {
    std::string requestBuffer;
    requestBuffer.resize(8192);
    const auto bytes = ::recv(fd, requestBuffer.data(), requestBuffer.size() - 1, 0);
    if(bytes <= 0) {
        return;
    }
    requestBuffer.resize(static_cast<std::size_t>(bytes));

    const auto request = parseRequestLine(requestBuffer);
    if(request.method == "GET" && request.path == "/camera/status") {
        const auto status = camera.status();
        sendTextResponse(fd, 200, "OK", "application/json", makeJsonResponse(camera, status));
    } else if(request.method == "POST" && request.path == "/camera/photo") {
        const auto status = camera.savePhoto();
        sendTextResponse(fd, status.success ? 200 : 500, status.success ? "OK" : "Internal Server Error", "application/json", makeJsonResponse(camera, status));
    } else if(request.method == "POST" && request.path == "/camera/record/start") {
        const auto status = camera.startRecording();
        sendTextResponse(fd, status.success ? 200 : 500, status.success ? "OK" : "Internal Server Error", "application/json", makeJsonResponse(camera, status));
    } else if(request.method == "POST" && request.path == "/camera/record/stop") {
        const auto status = camera.stopRecording();
        sendTextResponse(fd, 200, "OK", "application/json", makeJsonResponse(camera, status));
    } else if(request.method == "GET" && request.path == "/camera/stream.mjpeg") {
        handleMjpegStream(fd, camera, config);
    } else {
        sendTextResponse(fd, 404, "Not Found", "application/json", "{\"success\":false,\"message\":\"not found\",\"path\":\"\",\"cameraConnected\":false,\"recording\":false}");
    }

}

OakCameraConfig parseConfig(int argc, char** argv) {
    OakCameraConfig config;
    for(int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto requireValue = [&](const std::string& name) -> std::string {
            if(i + 1 >= argc) {
                throw std::runtime_error("Missing value for " + name);
            }
            return argv[++i];
        };
        auto integer = [&](const std::string& name) {
            const auto value = requireValue(name);
            std::size_t end = 0;
            const int result = std::stoi(value, &end);
            if(end != value.size()) throw std::invalid_argument("invalid integer for " + name);
            return result;
        };
        auto real = [&](const std::string& name) {
            const auto value = requireValue(name);
            std::size_t end = 0;
            const float result = std::stof(value, &end);
            if(end != value.size()) throw std::invalid_argument("invalid number for " + name);
            return result;
        };

        if(arg == "--bind") {
            config.bindAddress = requireValue(arg);
        } else if(arg == "--port") {
            config.port = integer(arg);
        } else if(arg == "--snapshot-dir") {
            config.snapshotDir = requireValue(arg);
        } else if(arg == "--recording-dir") {
            config.recordingDir = requireValue(arg);
        } else if(arg == "--width") {
            config.previewWidth = integer(arg);
        } else if(arg == "--height") {
            config.previewHeight = integer(arg);
        } else if(arg == "--fps") {
            config.previewFps = real(arg);
        } else if(arg == "--jpeg-quality") {
            config.jpegQuality = integer(arg);
        } else if(arg == "--frame-freshness-ms") {
            config.frameFreshnessMs = integer(arg);
        } else if(arg == "--no-frame-timeout-ms") {
            config.noFrameTimeoutMs = integer(arg);
        } else if(arg == "--reconnect-interval-ms") {
            config.reconnectIntervalMs = integer(arg);
        } else if(arg == "--reconnect-max-interval-ms") {
            config.reconnectMaxIntervalMs = integer(arg);
        } else if(arg == "--disable-low-latency-profile") {
            config.lowLatencyProfile = false;
        } else if(arg == "--help" || arg == "-h") {
            std::cout << "Usage: oak_camera_service [--bind ADDR] [--port PORT] [--snapshot-dir DIR] [--recording-dir DIR] [--width W] [--height H] [--fps FPS] [--jpeg-quality N] [--disable-low-latency-profile] [--frame-freshness-ms N] [--no-frame-timeout-ms N] [--reconnect-interval-ms N] [--reconnect-max-interval-ms N]\n";
            std::exit(0);
        } else {
            throw std::runtime_error("Unknown argument: " + arg);
        }
    }

    validateOakCameraConfig(config);
    if(config.lowLatencyProfile) {
        config.previewWidth = std::min(config.previewWidth, 960);
        config.previewHeight = std::min(config.previewHeight, 540);
        config.previewFps = std::min(config.previewFps, 15.0f);
        config.jpegQuality = std::min(config.jpegQuality, 70);
    }

    return config;
}

int makeServerSocket(const OakCameraConfig& config) {
    const int serverFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if(serverFd < 0) {
        throw std::runtime_error("socket() failed");
    }

    int opt = 1;
    ::setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    timeval timeout{};
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    ::setsockopt(serverFd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(config.port));
    if(::inet_pton(AF_INET, config.bindAddress.c_str(), &address.sin_addr) != 1) {
        ::close(serverFd);
        throw std::runtime_error("invalid bind address: " + config.bindAddress);
    }

    if(::bind(serverFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        const auto err = std::string(std::strerror(errno));
        ::close(serverFd);
        throw std::runtime_error("bind() failed: " + err);
    }

    if(::listen(serverFd, 16) != 0) {
        const auto err = std::string(std::strerror(errno));
        ::close(serverFd);
        throw std::runtime_error("listen() failed: " + err);
    }

    return serverFd;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto config = parseConfig(argc, argv);
        std::signal(SIGINT, signalHandler);
        std::signal(SIGTERM, signalHandler);
        std::signal(SIGPIPE, SIG_IGN);

        // Acquire the port before opening the camera, so duplicates do not steal it.
        const int serverFd = makeServerSocket(config);
        OakCameraDevice camera(config);
        try { camera.start(); }
        catch(...) { ::close(serverFd); throw; }
        std::vector<std::unique_ptr<ClientConnection>> clients;
        std::cout << "oak_camera_service listening on " << config.bindAddress << ':' << config.port << std::endl;

        while(gRunning.load()) {
            for(auto it = clients.begin(); it != clients.end();) {
                if((*it)->done.load()) {
                    (*it)->worker.join();
                    it = clients.erase(it);
                } else ++it;
            }
            sockaddr_in clientAddr{};
            socklen_t clientLen = sizeof(clientAddr);
            const int clientFd = ::accept(serverFd, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
            if(clientFd < 0) {
                if(errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                std::cerr << "oak_camera_service accept() failed: " << std::strerror(errno) << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            timeval clientTimeout{};
            clientTimeout.tv_sec = 2;
            ::setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &clientTimeout, sizeof(clientTimeout));
            ::setsockopt(clientFd, SOL_SOCKET, SO_SNDTIMEO, &clientTimeout, sizeof(clientTimeout));
            if(clients.size() >= 16) {
                sendTextResponse(clientFd, 503, "Service Unavailable", "application/json",
                                 "{\"success\":false,\"message\":\"too many camera clients\"}");
                ::close(clientFd);
                continue;
            }
            auto connection = std::make_unique<ClientConnection>();
            connection->fd = clientFd;
            auto* current = connection.get();
            clients.push_back(std::move(connection));
            try {
                current->worker = std::thread([current, &camera, &config] {
                    try { handleClient(current->fd, camera, config); }
                    catch(const std::exception& e) {
                        std::cerr << "camera HTTP client error: " << e.what() << std::endl;
                    }
                    catch(...) { std::cerr << "camera HTTP client unknown error" << std::endl; }
                    {
                        std::lock_guard<std::mutex> lock(current->mutex);
                        ::shutdown(current->fd, SHUT_RDWR);
                        ::close(current->fd);
                        current->fd = -1;
                    }
                    current->done.store(true);
                });
            } catch(...) {
                ::close(clientFd);
                clients.pop_back();
                gRunning.store(false);
                std::cerr << "cannot create camera HTTP worker" << std::endl;
            }
        }

        ::close(serverFd);
        for(const auto& client : clients) {
            std::lock_guard<std::mutex> lock(client->mutex);
            if(client->fd >= 0) ::shutdown(client->fd, SHUT_RDWR);
        }
        camera.stop();
        for(const auto& client : clients) client->worker.join();
        return 0;
    } catch(const std::exception& e) {
        std::cerr << "oak_camera_service error: " << e.what() << std::endl;
        return 1;
    }
}
