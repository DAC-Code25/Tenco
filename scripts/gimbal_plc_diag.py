#!/usr/bin/env python3
"""
Safe diagnostic tool for the Tenco gimbal PLC.

Default behavior is read-only. Any write operation requires --yes and is
implemented as a short pulse followed by automatic clearing.
"""

import argparse
import sys
import time
from dataclasses import dataclass
from typing import Iterable, List, Optional, Sequence

try:
    from pymodbus.client import ModbusTcpClient
except ImportError as exc:
    raise SystemExit(
        "pymodbus is required. Install it with: python -m pip install pymodbus"
    ) from exc


DEFAULT_HOST = "192.168.31.120"
DEFAULT_PORT = 502
DEFAULT_UNIT = 0xFF
STATUS_START = 100
STATUS_COUNT = 7


def to_signed_16(value: int) -> int:
    return value - 65536 if value > 32767 else value


def fmt_status(registers: Sequence[int]) -> str:
    if len(registers) < STATUS_COUNT:
        return f"status raw: {list(registers)}"

    signed_height = to_signed_16(registers[1])
    return (
        f"state={registers[0]} "
        f"height={signed_height}({registers[1]}) "
        f"yaw={registers[2]} "
        f"pitch={registers[3]} "
        f"x_motion={registers[4]} "
        f"z_motion={registers[5]} "
        f"set_vel={registers[6]}"
    )


@dataclass
class PlcClient:
    host: str
    port: int
    unit: int
    timeout: float

    def __post_init__(self) -> None:
        self.client = ModbusTcpClient(self.host, port=self.port, timeout=self.timeout)

    def __enter__(self) -> "PlcClient":
        if not self.client.connect():
            raise SystemExit(f"connect failed: {self.host}:{self.port}")
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.client.close()

    def read_holding(self, address: int, count: int) -> Optional[List[int]]:
        result = self.client.read_holding_registers(
            address=address, count=count, slave=self.unit
        )
        if result.isError():
            print(f"read holding error address={address} count={count}: {result}")
            return None
        return list(result.registers)

    def read_coils(self, address: int, count: int) -> Optional[List[bool]]:
        result = self.client.read_coils(address=address, count=count, slave=self.unit)
        if result.isError():
            print(f"read coil error address={address} count={count}: {result}")
            return None
        return [bool(value) for value in result.bits[:count]]

    def write_holding(self, address: int, values: Sequence[int]) -> bool:
        result = self.client.write_registers(
            address=address, values=list(values), slave=self.unit
        )
        if result.isError():
            print(f"write holding error address={address} values={list(values)}: {result}")
            return False
        return True

    def write_coil(self, address: int, value: bool) -> bool:
        result = self.client.write_coil(address=address, value=value, slave=self.unit)
        if result.isError():
            print(f"write coil error address={address} value={value}: {result}")
            return False
        return True


def require_yes(args: argparse.Namespace, action: str) -> None:
    if not args.yes:
        raise SystemExit(f"{action} is a write operation. Re-run with --yes if intentional.")


def print_status(plc: PlcClient) -> None:
    status = plc.read_holding(STATUS_START, STATUS_COUNT)
    if status is not None:
        print(fmt_status(status), flush=True)


def clear_outputs(plc: PlcClient, coil_range: Iterable[int]) -> None:
    plc.write_holding(0, [0, 0, 0, 0, 0, 0])
    for address in coil_range:
        plc.write_coil(address, False)
    print("clear command sent: holding 0..5 = 0, configured coils = false", flush=True)
    print_status(plc)


def pulse_holding(plc: PlcClient, address: int, value: int, duration_ms: int) -> None:
    print_status(plc)
    print(f"pulse holding address={address} value={value} duration_ms={duration_ms}", flush=True)
    if plc.write_holding(address, [value]):
        time.sleep(duration_ms / 1000.0)
        plc.write_holding(address, [0])
    print_status(plc)


def pulse_coil(plc: PlcClient, address: int, duration_ms: int) -> None:
    print_status(plc)
    print(f"pulse coil address={address} value=true duration_ms={duration_ms}", flush=True)
    if plc.write_coil(address, True):
        time.sleep(duration_ms / 1000.0)
        plc.write_coil(address, False)
    print_status(plc)


def watch(plc: PlcClient, interval: float, show_coils: bool, mark: str) -> None:
    last_status: Optional[List[int]] = None
    last_coils: Optional[List[bool]] = None
    heartbeat = 0

    if mark:
        print(f"mark: {mark}", flush=True)

    while True:
        status = plc.read_holding(STATUS_START, STATUS_COUNT)
        coils = plc.read_coils(0, 32) if show_coils else None

        changed = False
        if status is not None and status != last_status:
            print(time.strftime("%H:%M:%S"), fmt_status(status), flush=True)
            last_status = status
            changed = True

        if coils is not None and last_coils is not None and coils != last_coils:
            changes = [
                f"coil {index}: {old} -> {new}"
                for index, (old, new) in enumerate(zip(last_coils, coils))
                if old != new
            ]
            if changes:
                print(time.strftime("%H:%M:%S"), "; ".join(changes), flush=True)
                changed = True

        if coils is not None:
            last_coils = coils

        heartbeat += 1
        if not changed and heartbeat % max(1, int(2.0 / interval)) == 0 and last_status:
            print(time.strftime("%H:%M:%S"), fmt_status(last_status), flush=True)

        time.sleep(interval)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Tenco gimbal PLC safe diagnostic tool")
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--unit", type=lambda value: int(value, 0), default=DEFAULT_UNIT)
    parser.add_argument("--timeout", type=float, default=2.0)
    parser.add_argument("--watch", action="store_true", help="read status continuously")
    parser.add_argument("--interval", type=float, default=0.1, help="watch interval seconds")
    parser.add_argument("--show-coils", action="store_true", help="also print coil changes while watching")
    parser.add_argument("--mark", default="", help="print a label before watch output")
    parser.add_argument("--clear", action="store_true", help="clear holding 0..5 and coils 0..31")
    parser.add_argument("--yes", action="store_true", help="required for write operations")
    parser.add_argument(
        "--pulse-holding",
        nargs=2,
        metavar=("ADDRESS", "VALUE"),
        type=lambda value: int(value, 0),
        help="write one holding register briefly, then clear it",
    )
    parser.add_argument(
        "--pulse-coil",
        metavar="ADDRESS",
        type=lambda value: int(value, 0),
        help="write one coil true briefly, then clear it",
    )
    parser.add_argument("--duration-ms", type=int, default=100)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.duration_ms < 20 or args.duration_ms > 500:
        raise SystemExit("--duration-ms must be between 20 and 500")

    with PlcClient(args.host, args.port, args.unit, args.timeout) as plc:
        if args.clear:
            require_yes(args, "--clear")
            clear_outputs(plc, range(0, 32))

        if args.pulse_holding:
            require_yes(args, "--pulse-holding")
            address, value = args.pulse_holding
            pulse_holding(plc, address, value, args.duration_ms)

        if args.pulse_coil is not None:
            require_yes(args, "--pulse-coil")
            pulse_coil(plc, args.pulse_coil, args.duration_ms)

        if args.watch:
            watch(plc, args.interval, args.show_coils, args.mark)

        if not args.watch and not args.clear and not args.pulse_holding and args.pulse_coil is None:
            print_status(plc)

    return 0


if __name__ == "__main__":
    sys.exit(main())
