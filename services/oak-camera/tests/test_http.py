"""Exercise production HTTP/recovery code using a test-only capture source."""
import http.client
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time


def wait_for(predicate, reason, timeout=4):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        try:
            value = predicate()
            if value:
                return value
        except (OSError, http.client.HTTPException):
            pass
        time.sleep(0.02)
    raise AssertionError(reason)


with tempfile.TemporaryDirectory(prefix="oak-http-test-") as directory:
    state = Path(directory) / "state"
    state.write_text("missing")
    with socket.socket() as port_probe:
        port_probe.bind(("127.0.0.1", 0))
        port = port_probe.getsockname()[1]
    env = dict(os.environ, OAK_TEST_STATE_FILE=str(state))
    args = [sys.argv[1], "--bind", "127.0.0.1", "--port", str(port),
            "--snapshot-dir", directory + "/photos", "--recording-dir", directory + "/records",
            "--frame-freshness-ms", "100", "--no-frame-timeout-ms", "200",
            "--reconnect-interval-ms", "30", "--reconnect-max-interval-ms", "120"]
    log = open(Path(directory) / "service.log", "w+")
    process = subprocess.Popen(args, env=env, stdout=log, stderr=log)
    sockets = []

    def request(path, method="GET"):
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=2)
        connection.request(method, path)
        response = connection.getresponse()
        status, body = response.status, response.read()
        connection.close()
        return status, json.loads(body)

    def status():
        return request("/camera/status")[1]

    try:
        wait_for(lambda: status().get("reconnectCount", 0) > 0, "missing source not diagnosed")
        code, body = request("/camera/stream.mjpeg")
        assert code == 503 and not body["cameraConnected"]
        state.write_text("healthy")
        wait_for(lambda: status()["cameraConnected"], "HTTP capture did not become healthy")
        duplicate = subprocess.run(args, env=env, capture_output=True, timeout=3)
        assert duplicate.returncode != 0 and b"bind() failed" in duplicate.stderr
        assert b"capture connected" not in duplicate.stdout
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
        connection.request("GET", "/camera/stream.mjpeg")
        stream = connection.getresponse()
        assert stream.status == 200
        frames = []
        for _ in range(3):
            assert stream.readline().startswith(b"--tenco-oak-boundary")
            assert stream.readline().strip() == b"Content-Type: image/jpeg"
            length = int(stream.readline().split(b":")[1])
            assert stream.readline() == b"\r\n"
            jpeg = stream.read(length)
            assert jpeg.startswith(b"\xff\xd8") and jpeg.endswith(b"\xff\xd9")
            assert stream.read(2) == b"\r\n"
            frames.append(jpeg)
        assert len(set(frames)) > 1, "fixture frames did not change"
        old_id = status()["frameId"]
        state.write_text("fault")
        wait_for(lambda: not status()["cameraConnected"], "disconnect not reported")
        # Drain queued frames and verify finite EOF rather than an old image loop.
        assert len(stream.read()) < 1024 * 1024
        connection.close()
        code, body = request("/camera/stream.mjpeg")
        assert code == 503 and body["lastFrameAgeMs"] == -1
        assert "X_LINK_ERROR" in body["lastCaptureError"]
        state.write_text("healthy")
        wait_for(lambda: status()["cameraConnected"] and status()["frameId"] > old_id,
                 "service did not recover after fault")
        code, photo = request("/camera/photo", "POST")
        assert code == 200 and Path(photo["path"]).is_file()
        assert request("/camera/record/start", "POST")[0] == 200
        state.write_text("stall")
        wait_for(lambda: not status()["cameraConnected"], "stale stream remained healthy")
        wait_for(lambda: "stalled" in status()["lastCaptureError"], "stall did not cause rebuild")
        assert not status()["recording"]
        state.write_text("healthy")
        wait_for(lambda: status()["cameraConnected"], "service did not recover from stall")
        assert not status()["recording"]
        # Partial requests cannot block shutdown or create unbounded workers.
        for _ in range(18):
            sock = socket.create_connection(("127.0.0.1", port), timeout=2)
            sockets.append(sock)
        start = time.monotonic()
        process.terminate()
        assert process.wait(timeout=4) == 0
        assert time.monotonic() - start < 4
        log.flush(); log.seek(0)
        text = log.read()
        assert "test X_LINK_ERROR" in text and "preview stalled" in text and "retry in" in text
        invalid = subprocess.run([sys.argv[1], "--fps", "nan"], capture_output=True, timeout=3)
        assert invalid.returncode != 0 and b"invalid camera" in invalid.stderr
        invalid = subprocess.run([sys.argv[1], "--port", "18080junk"], capture_output=True, timeout=3)
        assert invalid.returncode != 0 and b"invalid integer" in invalid.stderr
        print("HTTP missing/healthy/multiframe/EOF/503/recovery/photo/record/stall/stop/config passed")
    finally:
        for sock in sockets:
            sock.close()
        if process.poll() is None:
            process.kill(); process.wait()
        log.close()
