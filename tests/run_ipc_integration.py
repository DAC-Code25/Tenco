"""Two-repository integration, all endpoints loopback; no vehicle connection."""
import argparse
import asyncio
import contextlib
import importlib.util
import os
import json
from pathlib import Path
import shutil
import subprocess
import tempfile


async def main(args):
    module_path = args.ipc_source / "tracking_node/test/service_integration.py"
    spec = importlib.util.spec_from_file_location("ipc_integration", module_path)
    ipc = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(ipc)
    with tempfile.TemporaryDirectory(prefix="tenco-qt-ipc-") as directory:
        root = Path(directory)
        source = args.ipc_source / "control_common/config/simulation"
        for file in source.glob("*.json"):
            shutil.copy(file, root / file.name)
        config = (source / "tracking.yaml").read_text(encoding="utf-8")
        config = config.replace("../../../build/simulation-store", str(root / "store").replace("\\", "/"))
        (root / "tracking.yaml").write_text(config, encoding="utf-8")
        plant = ipc.Plant(19130)
        photos = []
        async def camera_request(reader, writer):
            try:
                head = await reader.readuntil(b"\r\n\r\n")
                length = next(int(line.split(b":", 1)[1]) for line in head.split(b"\r\n") if line.lower().startswith(b"content-length:"))
                payload = json.loads(await reader.readexactly(length))
                assert head.startswith(b"POST /camera/photo ") and payload["eventId"]
                photos.append(payload["eventId"])
                body = b'{"success":true,"path":"simulated-checkpoint.jpg"}'
                writer.write(b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " + str(len(body)).encode() + b"\r\n\r\n" + body)
                await writer.drain()
            finally:
                writer.close()
                await writer.wait_closed()
        camera = await asyncio.start_server(camera_request, "127.0.0.1", 19133)
        flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
        async with camera, ipc.websockets.serve(plant.handler, "127.0.0.1", 19132, max_size=65536):
            with (root / "service.log").open("w", encoding="utf-8") as log:
                server = subprocess.Popen([str(args.server), str(root / "tracking.yaml"), "--stdin-stop"],
                                          stdin=subprocess.PIPE, stdout=log, stderr=log, creationflags=flags)
                run_plant = probe = None
                try:
                    for _ in range(100):
                        assert server.poll() is None, "IPC service exited"
                        try:
                            await ipc.http(19130, "/health")
                            break
                        except (OSError, asyncio.TimeoutError):
                            await asyncio.sleep(.05)
                    run_plant = asyncio.create_task(plant.run())
                    probe = await asyncio.create_subprocess_exec(str(args.client), creationflags=flags,
                        stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT)
                    output, _ = await asyncio.wait_for(probe.communicate(), 50)
                    print(output.decode("utf-8", errors="replace"), flush=True)
                    result = probe.returncode
                    assert result == 0, f"Qt probe failed: {result}"
                    # The actual Qt coordinator has closed all observer/session clients.
                    observer = ipc.Operator(19130)
                    completed = await observer.wait(lambda s: s["state"] in ("Completed", "Paused", "Fault"), 15)
                    assert completed["state"] == "Completed", completed
                    assert abs(plant.x - 1.2) <= .03 and abs(plant.y) <= .03
                    print("PASS autonomous completion after Qt process exit", flush=True)
                    assert not plant.invalid_packets, plant.invalid_packets
                    assert len(photos) == 1, f"photo execution count: {len(photos)}"
                finally:
                    if probe and probe.returncode is None:
                        probe.kill()
                        await probe.wait()
                    if run_plant:
                        run_plant.cancel()
                        with contextlib.suppress(asyncio.CancelledError):
                            await run_plant
                    if server.poll() is None:
                        server.stdin.write(b"stop\n")
                        server.stdin.flush()
                        try:
                            await asyncio.wait_for(asyncio.to_thread(server.wait), 5)
                        except asyncio.TimeoutError:
                            server.kill()
                            raise AssertionError("service failed to drain")
                    assert server.returncode == 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--ipc-source", type=Path, required=True)
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--client", type=Path, required=True)
    arguments = parser.parse_args()
    arguments.ipc_source = arguments.ipc_source.resolve()
    arguments.server = arguments.server.resolve()
    arguments.client = arguments.client.resolve()
    asyncio.run(main(arguments))
