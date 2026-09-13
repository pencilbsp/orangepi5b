#!/usr/bin/env python3
"""Switch GNOME/Mutter display color mode and print the resulting DRM state.

Run on the Orange Pi desktop session as the logged-in user:
  ./display-color-mode.py sdr --connector DP-1
  ./display-color-mode.py hdr --connector DP-1
  ./display-color-mode.py dump
"""
from __future__ import annotations

import argparse
import os
import subprocess
import time

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

DEST = "org.gnome.Mutter.DisplayConfig"
PATH = "/org/gnome/Mutter/DisplayConfig"
IFACE = "org.gnome.Mutter.DisplayConfig"

COLOR_MODES = {
    "sdr": 0,
    "hdr": 1,
}


def call(
    bus: Gio.DBusConnection,
    method: str,
    params: GLib.Variant | None = None,
) -> GLib.Variant:
    return bus.call_sync(
        DEST, PATH, IFACE, method, params, None, Gio.DBusCallFlags.NONE, 10000, None
    )


def get_state(bus: Gio.DBusConnection):
    return call(bus, "GetCurrentState").unpack()


def active_monitors(monitors):
    active = []
    for spec, modes, props in monitors:
        for mode in modes:
            if mode[6].get("is-current"):
                active.append((spec, modes, mode, props))
                break
    return active


def select_monitor(monitors, connector: str | None):
    active = active_monitors(monitors)
    if connector:
        for monitor in active:
            if monitor[0][0] == connector:
                return monitor
        available = ", ".join(item[0][0] for item in active) or "none"
        raise SystemExit(f"connector {connector!r} is not active; active connectors: {available}")
    if active:
        return active[0]
    raise SystemExit("no active monitor found")


def monitor_apply_props(monitor_props, color_mode: int | None = None):
    props = {}
    if color_mode is not None:
        props["color-mode"] = GLib.Variant("u", color_mode)
    elif "color-mode" in monitor_props:
        props["color-mode"] = GLib.Variant("u", monitor_props["color-mode"])
    if "rgb-range" in monitor_props:
        props["rgb-range"] = GLib.Variant("u", monitor_props["rgb-range"])
    return props


def apply_color_mode(
    bus: Gio.DBusConnection,
    color_mode: int,
    persistent: bool,
    requested_mode_id: str | None,
    requested_connector: str | None,
) -> None:
    serial, monitors, logical_monitors, global_props = get_state(bus)
    spec, modes, current_mode, _monitor_props = select_monitor(monitors, requested_connector)
    connector, _vendor, _product, _serial = spec
    mode_id = requested_mode_id or current_mode[0]
    if requested_mode_id and not any(mode[0] == requested_mode_id for mode in modes):
        available = ", ".join(mode[0] for mode in modes)
        raise SystemExit(f"mode {requested_mode_id!r} is unavailable; choices: {available}")
    monitor_state = {item[0][0]: item for item in active_monitors(monitors)}
    logical = []
    for x, y, scale, transform, primary, monitor_specs, _logical_props in logical_monitors:
        configs = []
        for monitor_spec in monitor_specs:
            current = monitor_state.get(monitor_spec[0])
            if current is None:
                raise SystemExit(f"logical monitor {monitor_spec[0]!r} has no active mode")
            _current_spec, _modes, active_mode, props = current
            is_target = monitor_spec[0] == connector
            configs.append(
                (
                    monitor_spec[0],
                    mode_id if is_target else active_mode[0],
                    monitor_apply_props(props, color_mode if is_target else None),
                )
            )
        logical.append((x, y, scale, transform, primary, configs))
    top_props = {"layout-mode": GLib.Variant("u", global_props.get("layout-mode", 1))}
    method = 2 if persistent else 1
    params = GLib.Variant(
        "(uua(iiduba(ssa{sv}))a{sv})", (serial, method, logical, top_props)
    )
    call(bus, "ApplyMonitorsConfig", params)


def dump_mutter(bus: Gio.DBusConnection) -> None:
    serial, monitors, _logical_monitors, global_props = get_state(bus)
    print(f"mutter_serial={serial} layout_mode={global_props.get('layout-mode')}")
    for spec, modes, props in monitors:
        current = [m for m in modes if m[6].get("is-current")]
        shown_props = (
            "display-name",
            "color-mode",
            "rgb-range",
            "supported-color-modes",
        )
        compact = {key: props[key] for key in props if key in shown_props}
        print(f"monitor={spec!r} props={compact!r}")
        for mode in current:
            print(f"current_mode={mode[0]} {mode[1]}x{mode[2]}@{mode[3]:.3f}")


def dump_kernel(connectors) -> None:
    state = "/sys/kernel/debug/dri/display-subsystem/state"
    if os.geteuid() != 0:
        sudo = ["sudo", "-n"]
    else:
        sudo = []
    keys = (
        "colorspace=",
        "broadcast_rgb=",
        "is_limited_range=",
        "output_bpc=",
        "output_format=",
        "tmds_char_rate=",
    )
    for connector in connectors:
        print(f"kernel_connector={connector}")
        result = subprocess.run(
            sudo + ["grep", "-n", "-A20", "-B2", connector, state],
            check=False,
            capture_output=True,
            text=True,
        )
        for line in result.stdout.splitlines():
            if connector in line or any(key in line for key in keys):
                print(line)
        if not connector.startswith("HDMI-"):
            continue
        for name in ("avi", "hdr_drm"):
            path = f"/sys/kernel/debug/dri/display-subsystem/{connector}/infoframes/{name}"
            print(f"infoframe={connector}/{name}")
            subprocess.run(
                sudo + ["od", "-An", "-tx1", "-N64", path], check=False
            )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=["sdr", "hdr", "dump"])
    parser.add_argument(
        "--connector", help="active connector to change, for example DP-1"
    )
    parser.add_argument("--mode-id", help="Mutter mode ID to apply with sdr/hdr")
    parser.add_argument("--persistent", action="store_true", help="save the mode in monitors.xml")
    parser.add_argument("--no-kernel-dump", action="store_true")
    args = parser.parse_args()

    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    if args.mode != "dump":
        apply_color_mode(
            bus,
            COLOR_MODES[args.mode],
            args.persistent,
            args.mode_id,
            args.connector,
        )
        time.sleep(2)
    dump_mutter(bus)
    if not args.no_kernel_dump:
        _serial, monitors, _logical, _props = get_state(bus)
        connectors = (
            [args.connector]
            if args.connector
            else [item[0][0] for item in active_monitors(monitors)]
        )
        dump_kernel(connectors)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
