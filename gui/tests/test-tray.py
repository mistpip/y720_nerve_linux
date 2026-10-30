#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise real D-Bus menu events against a mock-backend GUI on a private bus."""
import os
import signal
from pathlib import Path
import subprocess
import sys
import tempfile
import time

try:
    from gi.repository import Gio, GLib
except ImportError:
    print("SKIP: Python GObject bindings required for D-Bus tray integration test")
    raise SystemExit(77)

gui, fixture = map(str, map(Path, sys.argv[1:]))
bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
bus.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
              "RequestName", GLib.Variant("(su)", ("org.kde.StatusNotifierWatcher", 0)),
              None, Gio.DBusCallFlags.NONE, 2000, None)
registered = []
node = Gio.DBusNodeInfo.new_for_xml("""<node><interface name="org.kde.StatusNotifierWatcher">
<method name="RegisterStatusNotifierItem"><arg type="s" direction="in"/></method>
</interface></node>""")


def watcher(connection, sender, path, interface, method, parameters, invocation):
    registered.append(parameters.unpack()[0])
    invocation.return_value(None)


registration = bus.register_object("/StatusNotifierWatcher", node.interfaces[0], watcher, None, None)
context = GLib.MainContext.default()


def until(condition, seconds=8):
    deadline = time.monotonic() + seconds
    while not condition():
        if time.monotonic() > deadline:
            raise AssertionError("D-Bus tray/GUI timeout")
        while context.pending():
            context.iteration(False)
        time.sleep(0.005)


def find_quit(tree):
    identifier, properties, children = tree
    if properties.get("label") in ("退出", "Quit"):
        return identifier
    for child in children:
        result = find_quit(child.unpack() if isinstance(child, GLib.Variant) else child)
        if result is not None:
            return result
    return None


with tempfile.TemporaryDirectory(prefix="y720-tray-test-") as directory:
    directory = Path(directory)
    light = directory / "y720-backlight"
    light.symlink_to(fixture)
    config = directory / "y720-control"
    config.mkdir()
    (config / "settings.ini").write_text("[general]\nglobal_hotkey=false\nclose_to_tray=true\n")
    env = dict(os.environ, XDG_CONFIG_HOME=str(directory), Y720_FAN_BIN=fixture,
               Y720_BACKLIGHT_BIN=str(light), G_DEBUG="fatal-criticals")
    for method, hanging in (("Event", False), ("EventGroup", False), ("EventGroup", True)):
        registered.clear()
        pid_file = directory / "children"
        if pid_file.exists():
            pid_file.unlink()
        case_env = dict(env)
        if hanging:
            case_env.update(Y720_FIXTURE_HANG="1", Y720_FIXTURE_PIDS=str(pid_file))
        with tempfile.TemporaryFile(mode="w+") as log:
            process = subprocess.Popen([gui, "--hidden"], env=case_env, stdout=log, stderr=log,
                                       start_new_session=True)
            try:
                until(lambda: bool(registered) or process.poll() is not None)
                assert registered and process.poll() is None, "GUI exited before tray registration"
                if hanging:
                    until(lambda: pid_file.exists() and len(pid_file.read_text().splitlines()) >= 2)
                service = registered[-1]
                layout = bus.call_sync(service, "/MenuBar", "com.canonical.dbusmenu", "GetLayout",
                                       GLib.Variant("(iias)", (0, -1, [])), None,
                                       Gio.DBusCallFlags.NONE, 2000, None)
                quit_id = find_quit(layout.unpack()[1])
                assert quit_id is not None, "Quit missing from tray layout"
                event = (quit_id, "clicked", GLib.Variant("i", 0), 0)
                parameters = (GLib.Variant("(isvu)", event) if method == "Event" else
                              GLib.Variant("(a(isvu))", ([event],)))
                bus.call_sync(service, "/MenuBar", "com.canonical.dbusmenu", method, parameters,
                              None, Gio.DBusCallFlags.NONE, 2000, None)
                until(lambda: process.poll() is not None)
                log.seek(0)
                output = log.read()
                assert process.returncode == 0, output
                assert "AddressSanitizer" not in output and "runtime error:" not in output, output
                if hanging:
                    for pid in map(int, pid_file.read_text().splitlines()):
                        assert not Path(f"/proc/{pid}").exists(), f"CLI child {pid} survived GUI quit"
                print(f"PASS tray {method} (blocked CLI={hanging}): clean quit without hardware writes")
            finally:
                try:
                    os.killpg(process.pid, signal.SIGKILL)  # Only this test's new process group.
                except ProcessLookupError:
                    pass
                process.wait(timeout=3)
bus.unregister_object(registration)
