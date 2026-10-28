#!/usr/bin/env python3
"""Development-only mock of y720-fan / y720-backlight.

No hardware access. Used to exercise the GUI on machines without a Y720.
Set Y720_MOCK_ERROR to one of: read_only, permission, busy, write_uncertain,
timeout, invalid_json. State lives in $XDG_RUNTIME_DIR/y720-control-mock.json.
"""
from __future__ import annotations

import json
import os
import random
import sys
import time
from pathlib import Path

PROGRAM = "y720-backlight" if Path(sys.argv[0]).name.endswith("backlight") else "y720-fan"
STATE_PATH = Path(os.environ.get("XDG_RUNTIME_DIR", "/tmp")) / "y720-control-mock.json"
ERROR = os.environ.get("Y720_MOCK_ERROR", "")


def load_state() -> dict:
    defaults = {
        "mode": "auto",
        "target": 0,
        "mode_writes": 12,
        "target_writes": 3,
        "ramp_low": 0,
        "light": {"effect": "static", "colors": [0, 0, 0, 0], "brightness": [2, 2, 2, 2]},
    }
    try:
        data = json.loads(STATE_PATH.read_text())
        defaults.update(data)
        return defaults
    except Exception:
        return defaults


def save_state(state: dict) -> None:
    try:
        STATE_PATH.write_text(json.dumps(state))
    except Exception:
        pass


def emit(data: dict) -> None:
    print(json.dumps({
        "schema_version": 1,
        "program": PROGRAM,
        "version": "0.1",
        "ok": True,
        "data": data,
    }, ensure_ascii=False))
    sys.exit(0)


def fail(code: str, message: str, status: int, details: dict | None = None) -> None:
    print(json.dumps({
        "schema_version": 1,
        "program": PROGRAM,
        "version": "0.1",
        "ok": False,
        "error": {"code": code, "message": message, "details": details or {}},
    }, ensure_ascii=False), file=sys.stderr)
    sys.exit(status)


def fan_status(state: dict) -> dict:
    if ERROR == "read_only":
        controls = False
    else:
        controls = True
    mode = state["mode"]
    target = state["target"]
    if not controls:
        rpm = [1790, 1760]
    elif mode == "boost":
        rpm = [5000 + random.randint(-25, 10), 4950 + random.randint(-20, 10)]
    elif mode == "curve":
        base = target or 3000
        rpm = [max(0, base + random.randint(-80, 40)), max(0, base + random.randint(-90, 30))]
    else:
        rpm = [1790 + random.randint(-15, 15), 1760 + random.randint(-15, 15)]
    return {
        "device": "hwmon6",
        "driver_version": "0.4.2",
        "controls_available": controls,
        "mode": mode if controls else None,
        "mode_raw": {"boost": 0, "curve": 1, "auto": 2}.get(mode) if controls else None,
        "target_rpm": target if controls else None,
        "pwm": (max(0, min(255, (target - 1500) * 255 // 3600)) if controls else None),
        "fan_rpm": rpm,
        "diagnostics": {
            "requested_mode": {"boost": 0, "curve": 1, "auto": 2}.get(mode) if controls else -1,
            "safety_fault": 0,
            "last_error": -5 if ERROR == "write_uncertain" else 0,
            "target_writes": state["target_writes"],
            "mode_writes": state["mode_writes"],
            "ramp_low_samples": state["ramp_low"],
        },
    }


def fan(args: list[str]) -> None:
    state = load_state()
    json_mode = "--json" in args
    dry = "--dry-run" in args
    filtered = [a for a in args if a not in ("--json", "--dry-run")]
    command = filtered[0] if filtered else "status"

    if not json_mode:
        fail("usage", "mock requires --json", 2)

    if ERROR == "permission" and command != "status":
        fail("permission", "Permission denied opening sysfs attribute", 4)
    if ERROR == "busy" and command != "status":
        fail("busy", "Another controller holds /run/y720-fan.lock", 5)
    if ERROR == "timeout" and command == "set":
        fail("timeout", "RPM did not settle; latest request remains, no rollback", 8)
    if ERROR == "write_uncertain" and command in ("set", "mode"):
        fail("write_uncertain", "write sysfs: Input/output error", 7,
             {"state_uncertain": True})
    if ERROR == "invalid_json":
        print("not json", file=sys.stdout)
        sys.exit(0)

    if command == "status":
        emit(fan_status(state))

    if command == "mode":
        mode = filtered[1] if len(filtered) > 1 else ""
        if mode not in ("auto", "boost"):
            fail("usage", "mode requires auto or boost", 2)
        state["mode"] = mode
        state["target"] = 0
        state["mode_writes"] += 1
        save_state(state)
        status = fan_status(state)
        emit({"operation": "mode", "request": {"mode": mode}, "status": status,
              "settled": False})

    if command == "set":
        rpm = None
        for i, arg in enumerate(filtered):
            if arg == "--rpm" and i + 1 < len(filtered):
                rpm = int(filtered[i + 1])
            elif arg.startswith("--rpm="):
                rpm = int(arg.split("=", 1)[1])
        if rpm is None:
            fail("usage", "set requires --rpm", 2)
        rpm = ((rpm + 99) // 100) * 100
        rpm = max(1500, min(5100, rpm))
        state["mode"] = "curve"
        state["target"] = rpm
        state["target_writes"] += 1
        save_state(state)
        status = fan_status(state)
        emit({"operation": "set",
              "request": {"target_rpm": rpm, "pwm": (rpm - 1500) * 255 // 3600,
                          "kind": "rpm", "value": rpm},
              "status": status, "settled": False})

    fail("usage", "unknown command", 2)


def backlight_profile(p: dict) -> dict:
    return {
        "schema_version": 1,
        "effect": p.get("effect", "static"),
        "colors": p.get("colors", [0, 0, 0, 0]),
        "brightness": p.get("brightness", [0, 0, 0, 0]),
    }


def backlight(args: list[str]) -> None:
    state = load_state()
    json_mode = "--json" in args
    dry = "--dry-run" in args
    filtered = [a for a in args if a not in ("--json", "--dry-run")]
    command = filtered[0] if filtered else "status"

    if not json_mode:
        fail("usage", "mock requires --json", 2)
    if ERROR == "permission" and command != "status":
        fail("permission", "Permission denied opening hidraw", 4)
    if ERROR == "busy" and command != "status":
        fail("busy", "Another controller holds /run/y720-backlight.lock", 5)
    if ERROR == "invalid_json":
        print("not json")
        sys.exit(0)

    if command in ("status", "info"):
        emit({
            "device": "hidraw1",
            "hid_device": "ITE33D1:00 048D:837A",
            "driver": "hid-generic",
            "descriptor_sha256": "587474323e725222dfa27cb66742b145b0f244e089cf2649f7d6bc0f86f50281",
            "report_bytes": 7,
            "state_known": False,
            "profile": None,
        })

    if command == "off":
        prof = {"effect": "static", "colors": [0, 0, 0, 0], "brightness": [0, 0, 0, 0]}
        state["light"] = prof
        save_state(state)
        emit({"profile": backlight_profile(prof), "reports_completed": 5,
              "transport_accepted": True, "visual_verified": False, "state_known": False})

    if command == "apply":
        raw = sys.stdin.read(4096)
        try:
            prof = json.loads(raw)
        except Exception:
            fail("usage", "Expected UTF-8 JSON profile schema 1", 2)
        if ERROR == "write_uncertain":
            fail("write_uncertain", "HID feature report failed", 7,
                 {"reports_completed": 3, "state_uncertain": True})
        state["light"] = prof
        save_state(state)
        if dry:
            emit({"profile": backlight_profile(prof),
                  "reports_hex": ["cc 00 03 00 02 00 00"] * 5,
                  "device_verified": False, "hardware_written": False})
        emit({"profile": backlight_profile(prof), "reports_completed": 5,
              "transport_accepted": True, "visual_verified": False, "state_known": False})

    fail("usage", "unknown command", 2)


def main() -> None:
    args = sys.argv[1:]
    if PROGRAM == "y720-backlight":
        backlight(args)
    else:
        fan(args)


if __name__ == "__main__":
    main()
