# Building

## Requirements

* Windows 10/11 x64.
* Python 3.9+ (only for the manual build and for the builder itself).
* MinGW-w64 x86_64 with `mingw32-make`.
  The simplest source is [winlibs.com](https://winlibs.com/) — unpack it
  into `C:\tools\mingw64`.
* CMake 3.20+.

Check it:

```powershell
gcc --version
mingw32-make --version
cmake --version
```

PATH must provide `x86_64-w64-mingw32-gcc` specifically (check with
`gcc -dumpmachine`).

## Builder

### EXE

```powershell
python -m pip install pyinstaller
powershell -ExecutionPolicy Bypass -File builder\build_exe.ps1
```

This produces `builder\dist\NovaPortBuilder.exe`. `tools/armrecomp.py`,
`runtime/` and the interface page (`builder/web/`) are packed inside, so
nothing needs to sit next to the EXE.

Running it without arguments opens the graphical interface in the browser,
`--cli` selects the old console mode, and `--gui` opens the interface
explicitly.

### Script

```powershell
python -m pip install pyelftools capstone
python builder\nova_build.py --apk game.apk --data cache\nova --out NovaPC
```

The script without flags asks the same questions in the console.

Flags:

| Flag | Meaning |
|---|---|
| `--apk` | the original 1.1.4 APK |
| `--so` | a ready `libnova.so` instead of the APK |
| `--data` | the cache folder (`*.pack`, `sounds/`) |
| `--out` | output folder, `NovaPC` by default |
| `--mingw` | MinGW folder (the one with `bin\gcc.exe`) |
| `--cmake` | path to `cmake.exe` |
| `--jobs` | number of build jobs |
| `--force` | overwrite a non-empty folder |
| `--keep-work` | keep `.build` (generated and object files) |
| `--no-strip` | do not strip debug symbols from `Nova.exe` |
| `--check` | check the tools and embedded files, then exit |
| `--translate-only` | translate ARM to C only, without building |
| `--gui` | open the graphical interface (default) |
| `--cli` | console mode; from the EXE this is what attaches to a console |

The builder finds the tools in PATH and in the usual places itself, and it
checks the compiler's bit width and the CMake version.

## Manual build

```powershell
# translate ARM -> C (needs pyelftools and capstone)
python tools\armrecomp.py --so libnova.so --out generated

# configure
cmake -S runtime -B build -G "MinGW Makefiles" `
      -DCMAKE_BUILD_TYPE=RelWithDebInfo `
      -DNOVA_GENERATED_DIR="$PWD\generated" `
      -DNOVA_OUTPUT_DIR="$PWD\out"

# the game only (without the console nova.exe)
cmake --build build --target NovaGame --parallel 8
```

CMake variables:

| Variable | Default | Purpose |
|---|---|---|
| `NOVA_GENERATED_DIR` | `runtime/generated` | where the translation output lives |
| `NOVA_OUTPUT_DIR` | `runtime` | where to put `Nova.exe` |
| `CMAKE_BUILD_TYPE` | `RelWithDebInfo` | build type |

The console `nova.exe` (target `nova`) is handy for debug runs: it writes to
stderr.

## Testing without the game

```powershell
$env:RECOMP_MAX_FRAMES = "300"
$env:NOVA_DATA_DIR = "D:\...\nova"
.\out\Nova.exe
```

The port loads the menu and exits. Look for errors in
`%LOCALAPPDATA%\Nova\nova.log`.

## Common problems

**"Build tools were not found"** — MinGW or CMake is not on PATH. Pass
`--mingw` and `--cmake` explicitly.

**CMake picks MSVC instead of MinGW** — do not use the Visual Studio
generator; the builder always passes `-G "MinGW Makefiles"` and the gcc
path.

**Translation fails on the `elftools`/`capstone` import** — install the
packages: `python -m pip install pyelftools capstone`. Inside the EXE they
are already bundled.

**The build fails with a compile error in `functions_*.c`** — that is a
translator bug. Do not edit the generated C: fix `tools/armrecomp.py` and
regenerate.

**The game starts but the screen is black** — check that `data\` sits next
to `Nova.exe` and contains `*.pack`, and that `libnova.so` is there too.
