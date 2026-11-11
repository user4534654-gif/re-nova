#!/usr/bin/env python3
"""Local web UI for the N.O.V.A. HD port builder.

A tiny stdlib HTTP server on 127.0.0.1 serves builder/web/ and exposes a few
JSON endpoints.  The native file dialogs run on the main thread through
tkinter; the build itself is a child process so its output can be streamed
into the page.
"""

import json
import os
import queue
import subprocess
import sys
import threading
import time
import webbrowser
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

APP_TITLE = "N.O.V.A. HD - PC port builder"
CREATOR_URL = "https://www.media-encrypt.online/links"
STATIC_FILES = {
    "/": ("index.html", "text/html; charset=utf-8"),
    "/index.html": ("index.html", "text/html; charset=utf-8"),
    "/style.css": ("style.css", "text/css; charset=utf-8"),
    "/app.js": ("app.js", "text/javascript; charset=utf-8"),
}


def resource_root():
    if getattr(sys, "frozen", False):
        return Path(getattr(sys, "_MEIPASS", Path(sys.executable).parent))
    return Path(__file__).resolve().parent.parent


WEB_DIR = resource_root() / "builder" / "web"


class BuildJob:
    def __init__(self):
        self.lock = threading.Lock()
        self.lines = deque(maxlen=5000)
        self.count = 0
        self.state = "idle"
        self.step = "Ready to build"
        self.error = ""
        self.out = ""
        self.proc = None
        self.cancel = False

    def add(self, line):
        with self.lock:
            self.lines.append(line)
            self.count += 1
            if line.startswith("==> "):
                self.step = line[4:].strip()

    def snapshot(self, since):
        with self.lock:
            total = self.count
            first = total - len(self.lines)
            start = max(0, since - first)
            chunk = list(self.lines)[start:] if since < total else []
            return {
                "state": self.state,
                "step": self.step,
                "error": self.error,
                "out": self.out,
                "lines": chunk,
                "next": total,
            }


class GuiApp:
    def __init__(self):
        self.job = BuildJob()
        self.dialogs = queue.Queue()
        self.stop = threading.Event()
        self.dialog_ok = None
        self.port = 0
        self.server = None
        self.server_thread = None
        self.last_seen = time.time()

    def touch(self):
        self.last_seen = time.time()

    def _watchdog(self):
        """Close the server when the page has been gone for a long while."""
        while not self.stop.wait(30):
            if self.job.state == "running":
                continue
            if time.time() - self.last_seen > 900:
                self.stop.set()
                return

    # ---- dialogs (main thread) -------------------------------------------
    def pick(self, kind):
        if self.dialog_ok is False:
            return ""
        box = {}
        done = threading.Event()
        self.dialogs.put((kind, box, done))
        if not done.wait(timeout=300):
            return ""
        return box.get("path", "")

    def _dialog_loop(self):
        try:
            import tkinter as tk
            from tkinter import filedialog
        except Exception as exc:  # no tkinter in this Python
            self.dialog_ok = False
            self.job.add("GUI: tkinter is unavailable (%s); type the paths by hand" % exc)
            self.stop.wait()
            return

        root = tk.Tk()
        root.withdraw()
        try:
            root.attributes("-topmost", True)
        except Exception:
            pass
        self.dialog_ok = True

        while not self.stop.is_set():
            try:
                kind, box, done = self.dialogs.get(timeout=0.2)
            except queue.Empty:
                try:
                    root.update()
                except Exception:
                    break
                continue
            try:
                root.update()
                if kind == "apk":
                    path = filedialog.askopenfilename(
                        parent=root, title="Select the N.O.V.A. HD 1.1.4 APK",
                        filetypes=[("Android APK", "*.apk"), ("All files", "*.*")])
                elif kind == "so":
                    path = filedialog.askopenfilename(
                        parent=root, title="Select libnova.so",
                        filetypes=[("Android library", "*.so"), ("All files", "*.*")])
                else:
                    path = filedialog.askdirectory(
                        parent=root, title="Select a folder", mustexist=True)
                box["path"] = path or ""
            except Exception as exc:
                self.job.add("GUI: the dialog did not open (%s)" % exc)
                box["path"] = ""
            finally:
                done.set()
        try:
            root.destroy()
        except Exception:
            pass

    # ---- build ------------------------------------------------------------
    def cli_command(self):
        if getattr(sys, "frozen", False):
            return [sys.executable, "--cli"]
        return [sys.executable, "-u", str(Path(__file__).resolve().parent / "nova_build.py")]

    def start_build(self, body):
        if self.job.state == "running":
            return {"ok": False, "error": "A build is already running."}
        apk = (body.get("apk") or "").strip().strip('"')
        so = (body.get("so") or "").strip().strip('"')
        data = (body.get("data") or "").strip().strip('"')
        out = (body.get("out") or "").strip().strip('"')
        if not (apk or so):
            return {"ok": False, "error": "Pick the APK (or libnova.so)."}
        if not data:
            return {"ok": False, "error": "Pick the game cache folder."}
        if not out:
            return {"ok": False, "error": "Pick the folder to build the port into."}
        if not Path(data).is_dir():
            return {"ok": False, "error": "Game cache folder not found: %s" % data}

        cmd = self.cli_command()
        if so and not apk:
            cmd += ["--so", so]
        else:
            cmd += ["--apk", apk]
        cmd += ["--data", data, "--out", out]
        if body.get("force"):
            cmd.append("--force")
        if body.get("keep_work"):
            cmd.append("--keep-work")
        if body.get("no_strip"):
            cmd.append("--no-strip")
        jobs = body.get("jobs")
        if isinstance(jobs, int) and jobs > 0:
            cmd += ["--jobs", str(jobs)]

        with self.job.lock:
            self.job.lines.clear()
            self.job.count = 0
            self.job.state = "running"
            self.job.step = "Starting the builder"
            self.job.error = ""
            self.job.out = out
        self.job.cancel = False

        env = dict(os.environ)
        env["PYTHONUNBUFFERED"] = "1"
        env["PYTHONIOENCODING"] = "utf-8"
        try:
            proc = subprocess.Popen(
                cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                stdin=subprocess.DEVNULL, text=True, encoding="utf-8",
                errors="replace", bufsize=1, env=env)
        except OSError as exc:
            with self.job.lock:
                self.job.state = "error"
                self.job.error = "Could not start the builder: %s" % exc
            return {"ok": False, "error": self.job.error}

        self.job.proc = proc
        threading.Thread(target=self._reader, args=(proc,), daemon=True).start()
        return {"ok": True}

    def _reader(self, proc):
        try:
            for line in proc.stdout:
                self.job.add(line.rstrip("\r\n"))
        finally:
            rc = proc.wait()
            with self.job.lock:
                if self.job.cancel:
                    self.job.state = "cancelled"
                    self.job.step = "Stopped"
                elif rc == 0:
                    self.job.state = "done"
                    self.job.step = "Done"
                else:
                    self.job.state = "error"
                    self.job.step = "Build interrupted"
                    self.job.error = "The builder exited with code %d. See the log." % rc
            self.job.proc = None

    def cancel_build(self):
        proc = self.job.proc
        if not proc:
            return {"ok": False, "error": "No build is running."}
        self.job.cancel = True
        self.job.add("GUI: stopping the build...")
        try:
            subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)],
                           capture_output=True, timeout=20)
        except Exception:
            try:
                proc.kill()
            except Exception:
                pass
        return {"ok": True}

    # ---- server -----------------------------------------------------------
    def run(self):
        handler = make_handler(self)
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
        self.server.daemon_threads = True
        self.port = self.server.server_address[1]
        self.server_thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.server_thread.start()

        url = "http://127.0.0.1:%d/" % self.port
        print("NovaPortBuilder GUI:", url, flush=True)
        threading.Thread(target=self._watchdog, daemon=True).start()
        try:
            webbrowser.open(url)
        except Exception:
            pass

        self._dialog_loop()
        self.stop.set()
        try:
            self.server.shutdown()
        except Exception:
            pass
        return 0


def make_handler(app):
    class Handler(BaseHTTPRequestHandler):
        server_version = "NovaPortBuilder"

        def log_message(self, *args):
            pass

        def _send(self, code, body, ctype="application/json; charset=utf-8"):
            data = body.encode("utf-8") if isinstance(body, str) else body
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            try:
                self.wfile.write(data)
            except BrokenPipeError:
                pass

        def _json(self, obj, code=200):
            self._send(code, json.dumps(obj, ensure_ascii=False))

        def _body(self):
            try:
                n = int(self.headers.get("Content-Length", "0"))
            except ValueError:
                n = 0
            raw = self.rfile.read(n) if n else b"{}"
            try:
                return json.loads(raw.decode("utf-8") or "{}")
            except (ValueError, UnicodeDecodeError):
                return {}

        def do_GET(self):
            app.touch()
            parsed = urlparse(self.path)
            if parsed.path in STATIC_FILES:
                name, ctype = STATIC_FILES[parsed.path]
                path = WEB_DIR / name
                try:
                    self._send(200, path.read_bytes(), ctype)
                except OSError:
                    self._send(404, "not found", "text/plain; charset=utf-8")
                return
            if parsed.path == "/api/status":
                qs = parse_qs(parsed.query)
                try:
                    since = int(qs.get("since", ["0"])[0])
                except ValueError:
                    since = 0
                self._json(app.job.snapshot(since))
                return
            self._send(404, "not found", "text/plain; charset=utf-8")

        def do_POST(self):
            app.touch()
            parsed = urlparse(self.path)
            path = parsed.path
            if path == "/api/pick":
                kind = parse_qs(parsed.query).get("kind", ["dir"])[0]
                self._json({"path": app.pick(kind)})
                return
            if path == "/api/start":
                self._json(app.start_build(self._body()))
                return
            if path == "/api/cancel":
                self._json(app.cancel_build())
                return
            if path == "/api/open":
                out = (self._body().get("out") or app.job.out or "").strip()
                if out and Path(out).is_dir():
                    try:
                        os.startfile(out)  # noqa: S606 (Windows shell open)
                        self._json({"ok": True})
                    except OSError as exc:
                        self._json({"ok": False, "error": str(exc)})
                else:
                    self._json({"ok": False, "error": "That folder does not exist yet."})
                return
            if path == "/api/quit":
                self._json({"ok": True})
                app.stop.set()
                return
            if path == "/api/url":
                self._json({"url": CREATOR_URL})
                return
            self._json({"ok": False, "error": "unknown endpoint"}, 404)

    return Handler


def run_gui():
    return GuiApp().run()