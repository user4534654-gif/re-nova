# nova-recomp — Windows port of N.O.V.A. HD 1.1.4

Static recompilation of the Android release of **N.O.V.A. HD 1.1.4**
(`com.gameloft.android.TBFV.GloftNOHP.ML`) into an ordinary Windows
application.

The ARM code in `libnova.so` is translated to C, then MinGW compiles it
into `Nova.exe`. The finished game has no emulator and no interpreter: it
is a native x86-64 binary that runs the same logic the phone ran.

## Requirements

* Windows 10/11 x64.
* The original **1.1.4 APK**.
* The extracted **game cache** (the `nova` folder with `*.pack` and
  `sounds/`).
* [MinGW-w64](https://winlibs.com/) (x86_64, with `mingw32-make`) and
  [CMake 3.20+](https://cmake.org/download/).

Game files are not part of this repository and are not distributed with it.

## Quick start

1. Build `NovaPortBuilder.exe` (a ready binary will appear under Releases
   later):

   ```powershell
   powershell -ExecutionPolicy Bypass -File builder\build_exe.ps1
   ```

2. Run `NovaPortBuilder.exe` by double-clicking it. The builder page opens
   in your browser (a local `127.0.0.1` address; nothing leaves the
   machine). Point it at:
   * the APK;
   * the game cache folder;
   * the folder to build the port into.

   The "Browse" buttons open the normal Explorer dialogs, the log streams
   into the page in real time, and "Stop" aborts the build. The "Shut the
   builder down" button at the bottom of the page closes the server.

3. Wait for it to finish (the first run is slow: about 15,900 functions
   are translated and roughly 200 large files are compiled). On 8 cores
   that takes about 20–25 minutes; the stripped `Nova.exe` is about 23 MB.

4. Run `play.cmd` in the port folder.

The same thing from the command line (console mode, no interface):

```powershell
NovaPortBuilder.exe --cli `
  --apk "D:\N.O.V.A.HD.1.1.4.apk" `
  --data "D:\N.O.V.A\gameloft\games\nova" `
  --out "D:\Games\NovaPC"
```

`--gui` opens the interface, `--cli` selects the console, and no arguments
mean the interface. `--cli --check` verifies the embedded files and the
toolchain.

From source, without the EXE:

```powershell
python builder\nova_build.py `
  --apk "D:\N.O.V.A.HD.1.1.4.apk" `
  --data "D:\N.O.V.A\gameloft\games\nova" `
  --out "D:\Games\NovaPC"
```

If MinGW or CMake are not on PATH:

```powershell
python builder\nova_build.py --apk ... --data ... --out ... `
  --mingw "C:\tools\mingw64" `
  --cmake "C:\Program Files\CMake\bin\cmake.exe"
```

## What you get

```
NovaPC\
  Nova.exe       the game
  libnova.so     the original library (needed at startup)
  data\          the game cache
  play.cmd       launcher
  README.txt     controls
```

`Nova.exe` needs `libnova.so` next to it: the ELF image and the symbol
table used for diagnostics are read from it.

## Controls

| Key | Action |
|---|---|
| mouse | look / aim |
| WASD | move |
| LMB | fire |
| Esc | pause / back |
| F11 or Alt+Enter | window / fullscreen |
| F2 | control hints |
| Caps Lock | drawing mode (multi-touch gestures) |
| wheel | off by default (`wheel_zoom = 1` enables it) |

Input settings live in `%LOCALAPPDATA%\Nova\input.ini`.
Saves live in `%LOCALAPPDATA%\Nova\`.
The log is `%LOCALAPPDATA%\Nova\nova.log`.

## Launch: window, intro, link

The game starts in a **bordered 1280×720 window**; F11 or Alt+Enter
toggles fullscreen. All of it is configured in
`%LOCALAPPDATA%\Nova\input.ini`:

```ini
[window]
width = 1280
height = 720
fullscreen = 0
movies = 1
open_link = 1
```

* `width` / `height` — window size in pixels.
* `fullscreen = 1` — start in fullscreen right away.
* `movies = 0` — do not play video (the boot intro and in-game clips).
* `open_link = 0` — do not open the creator's link on startup.

At startup the game plays the same intro the phone did: the engine asks
Java for `loadMovie("/sdcard/gameloft/games/nova/NOVA.mp4")`, and the port
finds that file in `data\` next to `Nova.exe` and plays it with sound. A
mouse click or Esc skips the clip. If `NOVA.mp4` is not in the cache, the
intro is simply skipped.

Once per launch, 2.5 seconds after the window appears, a browser opens the
mod creator's link — <https://www.media-encrypt.online/links>. To disable
it, set `open_link = 0` in `input.ini` or the environment variable
`NOVA_NO_LINK=1`.

Debug variables: `RECOMP_FULLSCREEN=1` (start in fullscreen),
`RECOMP_RES=1920x1080` / `RECOMP_WIDTH` / `RECOMP_HEIGHT`, `NOVA_NO_MOVIE=1`,
`NOVA_DATA_DIR=<cache folder>`, `RECOMP_MAX_FRAMES=<n>`.

## Manual build

```powershell
# 1. Translate
python tools\armrecomp.py --so libnova.so --out generated

# 2. Build
cmake -S runtime -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --target NovaGame -j 8

# 3. Run from the runtime tree
set NOVA_DATA_DIR=<cache folder>
.\Nova.exe
```

More detail in `docs/BUILDING.md`.

## Builder in GitHub Actions

`.github/workflows/build-builder.yml` builds `NovaPortBuilder.exe` on
`windows-latest` whenever `builder/`, `tools/`, `runtime/` or the workflow
itself changes, and also from the "Run workflow" button. It produces:

* the `NovaPortBuilder-windows-x64` artifact (kept for 14 days);
* the same file attached to a release when a tag such as `v1.0` is pushed.

That build needs no game files: only the translator, the runtime and the
interface page go into the EXE.

## Repository layout

```
tools/armrecomp.py     ARM/Thumb -> C translator
runtime/               runtime: CPU, memory, JNI, GLES, audio, input
builder/nova_build.py  builder "APK + cache -> port" (console and logic)
builder/nova_gui.py    the builder's local web interface
builder/web/           the interface page (HTML/CSS/JS)
builder/build_exe.ps1  packages the builder into a single EXE
.github/workflows/     EXE build and releases
docs/                  how it works and how to build
```

The interface is a local server on `127.0.0.1` with a random port: the page
opens in a normal browser, no files are uploaded anywhere, and the server
lives only while the builder is open.

## Limitations

* Only **N.O.V.A. HD 1.1.4** is supported. Other versions and games would
  need their own import and JNI shim work.
* The original game files are required. The repository contains only the
  recompiler and runtime code.
* The state has been verified on version 1.1.4 (ARMv7, VFPv2, no NEON).
* The sniper scope and some multi-touch gestures still need in-game
  testing.

## License

The repository code is MIT (`LICENSE`). Game assets and `libnova.so` belong
to their rights holders and are not part of the repository.
