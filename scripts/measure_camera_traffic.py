#!/usr/bin/env python3
"""Read-only MJPEG application-byte/FPS measurement; opens one extra stream."""
import argparse
import http.client
import json
import statistics
import time
from urllib.parse import urlsplit


def measure(url, seconds):
    endpoint = urlsplit(url)
    if endpoint.scheme not in ("http", "https") or not endpoint.hostname or endpoint.username or endpoint.password:
        raise ValueError("use an HTTP(S) stream URL without embedded credentials")
    connection_type = http.client.HTTPSConnection if endpoint.scheme == "https" else http.client.HTTPConnection
    connection = connection_type(endpoint.hostname, endpoint.port, timeout=5)
    try:
        path = endpoint.path or "/"
        if endpoint.query:
            path += "?" + endpoint.query
        connection.request("GET", path, headers={"Accept": "multipart/x-mixed-replace"})
        response = connection.getresponse()
        if response.status != 200 or "multipart/x-mixed-replace" not in response.getheader("Content-Type", ""):
            raise RuntimeError(f"expected MJPEG stream, HTTP {response.status}")
        sizes, frame_times = [], []
        received = 0
        start = time.monotonic()
        while time.monotonic() - start < seconds:
            boundary = response.readline(8193)
            received += len(boundary)
            if not boundary or len(boundary) > 8192 or not boundary.startswith(b"--"):
                raise RuntimeError("stream ended or invalid multipart boundary")
            headers = {}
            header_bytes = 0
            while True:
                line = response.readline(8193)
                received += len(line)
                header_bytes += len(line)
                if not line or header_bytes > 16384:
                    raise RuntimeError("incomplete or oversized multipart headers")
                if line == b"\r\n":
                    break
                name, value = line.decode("ascii").split(":", 1)
                headers[name.lower()] = value.strip()
            length = int(headers["content-length"])
            if not 0 < length <= 16 * 1024 * 1024:
                raise RuntimeError("invalid JPEG length")
            jpeg = response.read(length)
            trailer = response.read(2)
            if len(jpeg) != length or not jpeg.startswith(b"\xff\xd8") or not jpeg.endswith(b"\xff\xd9") or trailer != b"\r\n":
                raise RuntimeError("incomplete JPEG frame")
            received += len(jpeg) + len(trailer)
            sizes.append(length)
            frame_times.append(time.monotonic())
        elapsed = time.monotonic() - start
        # First frame may already be cached at connect time. Inter-arrival FPS
        # describes cadence; frames/elapsed also includes that initial frame.
        cadence = (len(sizes) - 1) / (frame_times[-1] - frame_times[0]) if len(sizes) > 1 else 0
        return {
            "scope": "one additional MJPEG stream; HTTP multipart body only; excludes TCP/IP/Wi-Fi overhead",
            "durationSeconds": elapsed, "frames": len(sizes), "cadenceFps": cadence,
            "multipartBytes": received, "meanJpegBytes": statistics.mean(sizes) if sizes else 0,
            "applicationMbps": received * 8 / elapsed / 1_000_000,
            "decimalGBPerHour": received / elapsed * 3600 / 1_000_000_000,
        }
    finally:
        connection.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", required=True, help="MJPEG stream URL")
    parser.add_argument("--seconds", type=float, default=15)
    args = parser.parse_args()
    if not 1 <= args.seconds <= 60:
        parser.error("--seconds must be between 1 and 60")
    print(json.dumps(measure(args.url, args.seconds), ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
