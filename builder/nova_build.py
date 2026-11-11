#!/usr/bin/env python3
"""N.O.V.A. HD 1.1.4 -> Windows PC port builder.

Takes the original game APK and the extracted game cache, translates
libnova.so into C and compiles the result with the user's MinGW-w64 + CMake.

Only the Python standard library is used here.  The translator itself needs
pyelftools and capstone; when this script runs from a frozen EXE those are
bundled, otherwise they must be installed with pip.
"""

import argparse
import importlib.util
import os
import re
import shutil
import subprocess
import sys
import time
import zipfile
from pathlib import Path

APP = "N.O.V.A. HD 1.1.4"
SO_IN_APK = "lib/armeabi-v7a/libnova.so"
SO_SHA256 = "a04448706c1e2b62fa880845ac5f36c09c3f0ab24a26c3ac309aab25b1f57e43"
DEFAULT_OUT = "NovaPC"


class BuildError(Exception):
    pass


def say(msg=""):
    print(msg, flush=True)


def step(msg):
    say()
    say("==> " + msg)


def resource_root():
    """Folder with tools/ and runtime/ (repo root, or the PyInstaller bundle)."""
    if getattr(sys, "frozen", False):
        return Path(getattr(sys, "_MEIPASS", Path(sys.executable).parent))
    return Path(__file__).resolve().parent.parent


def human_mb(path):
    try:
        return "%.1f MB" % (Path(path).stat().st_size / (1024 * 1024))
    except OSError:
        return "?"


def run(cmd, what):
    say("  " + " ".join(str(c) for c in cmd))
    try:
        code = subprocess.call([str(c) for c in cmd])
    except OSError as exc:
        raise BuildError("%s: could not start %s (%s)" % (what, cmd[0], exc))
    if code != 0:
        raise BuildError("%s exited with code %d" % (what, code))


# --------------------------------------------------------------------------
# toolchain discovery
# --------------------------------------------------------------------------

def _version_tuple(text):
    m = re.search(r"(\d+)\.(\d+)(?:\.(\d+))?", text)
    if not m:
        return (0, 0, 0)
    return tuple(int(x) for x in m.groups() if x is not None) + (0,) * (3 - len(m.groups()))


def find_cmake(explicit, extra_bins=()):
    cands = []
    if explicit:
        p = Path(explicit)
        cands += [p, p / "bin" / "cmake.exe"]
    found = shutil.which("cmake")
    if found:
        cands.append(Path(found))
    for b in extra_bins:
        cands.append(Path(b) / "cmake.exe")
    for base in (os.environ.get("ProgramFiles"), os.environ.get("ProgramFiles(x86)"),
                 os.environ.get("LOCALAPPDATA")):
        if base:
            cands.append(Path(base) / "CMake" / "bin" / "cmake.exe")
    cands += [Path(r"C:\tools\cmake\bin\cmake.exe"),
              Path(r"C:\tools\mingw64\bin\cmake.exe"),
              Path(r"C:\msys64\mingw64\bin\cmake.exe")]

    seen = set()
    for p in cands:
        key = str(p).lower()
        if key in seen or not p.is_file():
            continue
        seen.add(key)
        try:
            out = subprocess.check_output([str(p), "--version"], text=True,
                                          stderr=subprocess.DEVNULL,
                                          timeout=15)
        except (OSError, subprocess.SubprocessError):
            continue
        ver = _version_tuple(out)
        if ver >= (3, 20, 0):
            return p, out.splitlines()[0]
        say("  found CMake %s, but 3.20 or newer is required: %s" % (".".join(map(str, ver)), p))
    return None, None


def find_mingw(explicit):
    bins = []
    if explicit:
        p = Path(explicit)
        bins += [p, p / "bin"]
    found = shutil.which("gcc")
    if found:
        bins.append(Path(found).parent)
    bins += [
        Path(r"C:\tools\mingw64\bin"),
        Path(r"C:\msys64\mingw64\bin"),
        Path(r"C:\mingw64\bin"),
        Path(r"C:\mingw32\bin"),
    ]
    for base in (os.environ.get("ProgramFiles"), os.environ.get("ProgramFiles(x86)")):
        if base:
            for d in Path(base).glob("mingw-w64*/**/bin"):
                bins.append(d)

    seen = set()
    for b in bins:
        key = str(b).lower()
        if key in seen:
            continue
        seen.add(key)
        gcc = b / "gcc.exe"
        if not gcc.is_file():
            continue
        try:
            target = subprocess.check_output([str(gcc), "-dumpmachine"], text=True,
                                             stderr=subprocess.DEVNULL, timeout=15).strip()
            ver_out = subprocess.check_output([str(gcc), "--version"], text=True,
                                              stderr=subprocess.DEVNULL, timeout=15)
        except (OSError, subprocess.SubprocessError):
            continue
        if "x86_64" not in target or "mingw" not in target:
            say("  skipping %s: the compiler is not x86_64-w64-mingw32 (%s)" % (b, target))
            continue
        make = None
        for name in ("mingw32-make.exe", "make.exe"):
            if (b / name).is_file():
                make = b / name
                break
        if make is None:
            say("  skipping %s: no mingw32-make.exe" % b)
            continue
        return {"bin": b, "gcc": gcc, "make": make, "version": ver_out.splitlines()[0]}
    return None


TOOL_HINT = """\
Build tools were not found.

Required:
  * MinGW-w64 (x86_64, with mingw32-make) - for example https://winlibs.com/
    Unpack it, say, into C:\\tools\\mingw64 and add C:\\tools\\mingw64\\bin to PATH.
  * CMake 3.20+ - https://cmake.org/download/

Or pass the paths explicitly:
  --mingw "C:\\tools\\mingw64"  --cmake "C:\\Program Files\\CMake\\bin\\cmake.exe"
"""


# --------------------------------------------------------------------------
# inputs
# --------------------------------------------------------------------------

def extract_so(apk, dest):
    if not apk.is_file():
        raise BuildError("APK not found: %s" % apk)
    try:
        zf = zipfile.ZipFile(apk)
    except (zipfile.BadZipFile, OSError) as exc:
        raise BuildError("could not open the APK as a ZIP: %s" % exc)
    with zf:
        names = zf.namelist()
        pick = SO_IN_APK if SO_IN_APK in names else None
        if pick is None:
            pick = next((n for n in names
                         if n.startswith("lib/") and n.endswith("/libnova.so")), None)
        if pick is None:
            raise BuildError(
                "the APK has no libnova.so.\n"
                "This looks like a different APK. Version %s is required "
                "(com.gameloft.android.TBFV.GloftNOHP.ML)." % APP)
        say("  using %s (%s)" % (pick, human_mb_from_zip(zf, pick)))
        dest.parent.mkdir(parents=True, exist_ok=True)
        with zf.open(pick) as src, open(dest, "wb") as dst:
            shutil.copyfileobj(src, dst, 1024 * 1024)
    return dest


def human_mb_from_zip(zf, name):
    info = zf.getinfo(name)
    return "%.1f MB" % (info.file_size / (1024 * 1024))


def resolve_cache(path):
    if not path.is_dir():
        raise BuildError("game cache folder not found: %s" % path)
    probes = [path, path / "nova", path / "gameloft" / "games" / "nova",
              path / "games" / "nova"]
    for p in probes:
        if not p.is_dir():
            continue
        if (p / "sounds").is_dir() or any(p.glob("*.pack")):
            return p
    raise BuildError(
        "no game files were found in %s.\n"
        "Expected a folder with *.pack and sounds/ - usually\n"
        "  .../Android/data/com.gameloft.android.TBFV.GloftNOHP.ML/files/gameloft/games/nova\n"
        "or one of its parents." % path)


def copy_cache(src, dst):
    skip = {".svn", ".git", "__pycache__"}

    def ignore(_dir, names):
        return [n for n in names if n in skip]

    say("  copying %s -> %s" % (src, dst))
    shutil.copytree(src, dst, ignore=ignore, dirs_exist_ok=True)


# --------------------------------------------------------------------------
# translation and build
# --------------------------------------------------------------------------

def run_translator(so_path, out_dir, chunk_bytes):
    script = resource_root() / "tools" / "armrecomp.py"
    if not script.is_file():
        raise BuildError("tools/armrecomp.py was not found next to the builder")
    try:
        import elftools  # noqa: F401
        import capstone  # noqa: F401
    except ImportError as exc:
        raise BuildError(
            "the translator needs the pyelftools and capstone packages.\n"
            "Install them with:  python -m pip install pyelftools capstone\n"
            "(%s)" % exc)

    from importlib.machinery import SourceFileLoader
    loader = SourceFileLoader("nova_armrecomp", str(script))
    spec = importlib.util.spec_from_loader("nova_armrecomp", loader)
    mod = importlib.util.module_from_spec(spec)
    old_argv = sys.argv
    sys.argv = ["armrecomp.py",
                "--so", str(so_path),
                "--out", str(out_dir),
                "--chunk-bytes", str(chunk_bytes)]
    t0 = time.time()
    try:
        loader.exec_module(mod)
        mod.main()
    finally:
        sys.argv = old_argv
    say("  translation: %.1f s" % (time.time() - t0))


def build_port(tools, runtime_dir, gen_dir, build_dir, out_dir, jobs):
    cmake = tools["cmake"]
    mingw = tools["mingw"]
    cmd = [
        cmake, "-S", runtime_dir, "-B", build_dir,
        "-G", "MinGW Makefiles",
        "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
        "-DCMAKE_C_COMPILER=" + str(mingw["gcc"]),
        "-DCMAKE_MAKE_PROGRAM=" + str(mingw["make"]),
        "-DNOVA_GENERATED_DIR=" + str(gen_dir),
        "-DNOVA_OUTPUT_DIR=" + str(out_dir),
    ]
    run(cmd, "CMake configure")
    run([cmake, "--build", build_dir, "--target", "NovaGame",
         "--parallel", str(jobs)], "build")


def strip_exe(exe, mingw):
    strip = mingw["bin"] / "strip.exe"
    if not strip.is_file():
        return
    before = human_mb(exe)
    run([strip, "--strip-all", exe], "strip")
    say("  Nova.exe: %s -> %s" % (before, human_mb(exe)))


# --------------------------------------------------------------------------
# output
# --------------------------------------------------------------------------

PLAY_CMD = """@echo off
setlocal
cd /d "%~dp0"
set "NOVA_DATA_DIR=%~dp0data"
start "" "%~dp0Nova.exe"
"""

OUT_README = """N.O.V.A. HD 1.1.4 - Windows port
================================

Run: play.cmd (or Nova.exe).

What sits next to the game:
  Nova.exe     - the game
  libnova.so   - the original game library (the port needs it at startup)
  data\\        - the game cache
  play.cmd     - starts the game with the right data folder

Controls:
  mouse        - look / aim
  WASD         - move
  LMB          - fire
  Esc          - pause / back
  F11, Alt+Enter - window / fullscreen
  F2           - control hints
  Caps Lock    - drawing mode (multi-touch)
  wheel        - off by default

Input settings: %LOCALAPPDATA%\\Nova\\input.ini
Saves:          %LOCALAPPDATA%\\Nova\\
Log:            %LOCALAPPDATA%\\Nova\\nova.log

Game files are not part of the builder: you need your own APK and cache.
"""


def write_output(out_dir, so_path, source_cache):
    out_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(so_path, out_dir / "libnova.so")
    (out_dir / "play.cmd").write_text(PLAY_CMD, encoding="ascii", newline="\r\n")
    (out_dir / "README.txt").write_text(OUT_README, encoding="utf-8-sig",
                                        newline="\r\n")
    if not (out_dir / "data").is_dir():
        copy_cache(source_cache, out_dir / "data")


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------

def parse_args(argv):
    ap = argparse.ArgumentParser(
        prog="NovaPortBuilder",
        description="Builds the Windows port of N.O.V.A. HD 1.1.4 from the APK "
                    "and the game cache. Running it without arguments opens the "
                    "graphical interface; --cli selects the console mode.")
    ap.add_argument("--apk", help="path to the original 1.1.4 APK")
    ap.add_argument("--so", help="use libnova.so directly instead of the APK (debugging)")
    ap.add_argument("--data", help="game cache folder (*.pack, sounds/)")
    ap.add_argument("--out", default=DEFAULT_OUT, help="where to build (default %(default)s)")
    ap.add_argument("--cmake", help="path to cmake.exe")
    ap.add_argument("--mingw", help="MinGW-w64 folder (the one with bin\\gcc.exe)")
    ap.add_argument("--jobs", type=int, default=0, help="build jobs (default: all cores)")
    ap.add_argument("--chunk-bytes", type=int, default=500000,
                    help="generator chunk size (default %(default)s)")
    ap.add_argument("--keep-work", action="store_true", help="keep the intermediate files")
    ap.add_argument("--no-strip", action="store_true", help="do not strip debug symbols from Nova.exe")
    ap.add_argument("--force", action="store_true", help="overwrite a non-empty output folder")
    ap.add_argument("--check", action="store_true",
                    help="check the tools and the embedded files, then exit")
    ap.add_argument("--translate-only", action="store_true",
                    help="translate ARM to C only (diagnostics)")
    return ap.parse_args(argv)


def interactive(args):
    say("=" * 62)
    say(" PC port builder for %s" % APP)
    say("=" * 62)
    say("You need the original APK and the extracted game cache.")
    if not args.apk and not args.so:
        while True:
            p = input("APK (or a path to libnova.so): ").strip().strip('"')
            if p:
                break
        if p.lower().endswith(".so"):
            args.so = p
        else:
            args.apk = p
    if not args.data:
        args.data = input("Game cache folder (*.pack, sounds/): ").strip().strip('"')
    out = input("Build into [%s]: " % DEFAULT_OUT).strip().strip('"')
    if out:
        args.out = out
    return args


def ensure_console():
    """Attach a console when the frozen windowed EXE is run from cmd.exe."""
    if sys.stdout is not None and sys.stderr is not None:
        return
    try:
        import ctypes
        if ctypes.windll.kernel32.AttachConsole(-1):
            sys.stdout = open("CONOUT$", "w", encoding="utf-8", buffering=1)
            sys.stderr = open("CONOUT$", "w", encoding="utf-8", buffering=1)
            sys.stdin = open("CONIN$", "r", encoding="utf-8")
            return
    except Exception:
        pass
    if sys.stdout is None:
        sys.stdout = open(os.devnull, "w", encoding="utf-8")
    if sys.stderr is None:
        sys.stderr = open(os.devnull, "w", encoding="utf-8")


def check_environment():
    root = resource_root()
    say("builder: " + ("EXE" if getattr(sys, "frozen", False) else "script"))
    say("embedded files: %s" % root)
    ok = True
    for rel in ("tools/armrecomp.py", "runtime/CMakeLists.txt", "runtime/src/main.c"):
        p = root / rel
        say("  %-24s %s" % (rel, "ok" if p.is_file() else "MISSING"))
        ok = ok and p.is_file()
    try:
        import elftools
        import capstone
        say("  pyelftools               %s" % elftools.__version__)
        say("  capstone                 %s" % capstone.__version__)
    except ImportError as exc:
        say("  python packages          MISSING (%s)" % exc)
        ok = False
    mingw = find_mingw(None)
    if mingw:
        say("  MinGW                    %s" % mingw["version"])
    else:
        say("  MinGW                    not found")
        ok = False
    cmake, ver = find_cmake(None, [Path(mingw["bin"])] if mingw else [])
    if cmake:
        say("  CMake                    %s" % ver)
    else:
        say("  CMake                    not found")
        ok = False
    say()
    say("result: " + ("everything is in place" if ok else "components are missing"))
    return 0 if ok else 1


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    args = parse_args(argv)
    if args.check:
        return check_environment()
    if not argv:
        args = interactive(args)

    if not args.apk and not args.so:
        say("--apk or --so is required")
        return 2
    if not args.data and not args.translate_only:
        say("--data (the game cache folder) is required")
        return 2

    t_start = time.time()
    out_dir = Path(args.out).expanduser().resolve()
    work = out_dir / ".build"
    gen_dir = work / "generated"
    build_dir = work / "cmake"

    inputs = [Path(p).expanduser().resolve() for p in (args.apk, args.so, args.data) if p]
    for inp in inputs:
        if inp == out_dir or out_dir in inp.parents:
            raise BuildError(
                "the output folder %s overlaps the input data (%s).\n"
                "Pick another folder with --out." % (out_dir, inp))

    # Check the inputs before creating anything, so a typo does not leave a
    # half-made output folder that blocks the next attempt.
    src_so = Path(args.so).expanduser().resolve() if args.so else None
    src_apk = Path(args.apk).expanduser().resolve() if not args.so else None
    if src_so and not src_so.is_file():
        raise BuildError("libnova.so not found: %s" % src_so)
    if src_apk and not src_apk.is_file():
        raise BuildError("APK not found: %s" % src_apk)
    if not args.translate_only:
        src_data = Path(args.data).expanduser().resolve()
        if not src_data.exists():
            raise BuildError("game cache folder not found: %s" % src_data)

    if out_dir.exists() and any(out_dir.iterdir()):
        stale = [e for e in out_dir.iterdir() if e.name != ".build"]
        if not stale and not args.force:
            # only our own scratch folder is left from an earlier failed run
            shutil.rmtree(out_dir / ".build", ignore_errors=True)
        elif not args.force:
            raise BuildError(
                "the folder %s is not empty.\n"
                "Remove it, pick another one (--out) or pass --force." % out_dir)
        else:
            for entry in out_dir.iterdir():
                if entry.is_dir() and not entry.is_symlink():
                    shutil.rmtree(entry)
                else:
                    entry.unlink()
    out_dir.mkdir(parents=True, exist_ok=True)
    work.mkdir(parents=True, exist_ok=True)

    step("Checking the toolchain")
    mingw = find_mingw(args.mingw)
    if not mingw:
        raise BuildError(TOOL_HINT)
    say("  " + mingw["version"] + "  (" + str(mingw["gcc"]) + ")")
    say("  make: " + str(mingw["make"]))
    cmake_path, cmake_ver = find_cmake(args.cmake, [Path(mingw["bin"])])
    if not cmake_path:
        raise BuildError(TOOL_HINT)
    say("  " + cmake_ver + "  (" + str(cmake_path) + ")")
    tools = {"cmake": cmake_path, "mingw": mingw}

    step("Preparing the game files")
    so_path = work / "libnova.so"
    if src_so:
        shutil.copy2(src_so, so_path)
        say("  using the provided %s (%s)" % (src_so, human_mb(so_path)))
    else:
        extract_so(src_apk, so_path)
    source_cache = None
    if not args.translate_only:
        source_cache = resolve_cache(src_data)
        say("  cache: %s" % source_cache)

    step("Translating ARM to C (the longest step)")
    run_translator(so_path, gen_dir, args.chunk_bytes)

    if args.translate_only:
        say()
        say("Translation finished: %s" % gen_dir)
        say("Files: %d" % len(list(gen_dir.glob("functions_*.c"))))
        return 0

    step("Building Nova.exe (MinGW + CMake)")
    jobs = args.jobs if args.jobs > 0 else (os.cpu_count() or 4)
    build_port(tools, resource_root() / "runtime", gen_dir, build_dir, out_dir, jobs)

    exe = out_dir / "Nova.exe"
    if not exe.is_file():
        raise BuildError("the build finished but Nova.exe did not appear in %s" % out_dir)
    if not args.no_strip:
        step("Stripping the debug symbols")
        strip_exe(exe, mingw)

    step("Assembling the port folder")
    write_output(out_dir, so_path, source_cache)
    if not args.keep_work:
        shutil.rmtree(work, ignore_errors=True)

    say()
    say("Done in %.1f min." % ((time.time() - t_start) / 60.0))
    say("Port: %s" % out_dir)
    say("Run: %s" % (out_dir / "play.cmd"))
    return 0


def console(argv):
    """Run the command-line builder and return its exit code."""
    try:
        return main(argv)
    except BuildError as exc:
        say()
        say("ERROR: %s" % exc)
        return 1
    except KeyboardInterrupt:
        say("\nInterrupted.")
        return 130
    except Exception:
        import traceback
        say()
        say("UNEXPECTED ERROR:")
        traceback.print_exc()
        return 1


def run_gui():
    """Open the local web UI; fall back to the console if it cannot start."""
    try:
        from nova_gui import run_gui as gui_main
    except Exception as exc:
        ensure_console()
        say("The graphical interface is unavailable (%s)." % exc)
        say("Falling back to the console mode.")
        return console([])
    try:
        return gui_main()
    except Exception:
        import traceback
        traceback.print_exc()
        ensure_console()
        say("Could not open the graphical interface.")
        say("Start the builder with --cli instead.")
        return 1


def entry(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    gui = not argv or argv == ["--gui"]
    if gui:
        return run_gui()
    if argv and argv[0] == "--cli":
        argv = argv[1:]
    ensure_console()
    paused = not argv
    rc = console(argv)
    if paused:
        try:
            input("\nPress Enter to close...")
        except EOFError:
            pass
    return rc


if __name__ == "__main__":
    sys.exit(entry())
