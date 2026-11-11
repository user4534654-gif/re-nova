# How it works

## The idea

An Android game is an ordinary ARM binary, `lib/armeabi-v7a/libnova.so`,
which calls `libGLESv1_CM.so`, `libc.so`, `libm.so` and Java through JNI.
Instead of running ARM code in an emulator, the translator converts each
instruction into C once:

```
libnova.so (ARMv7)
      │  tools/armrecomp.py  (pyelftools + capstone)
      ▼
generated/functions_*.c   ~15,900 functions, ~97 MB of C
      │  MinGW-w64
      ▼
Nova.exe (x86-64)
```

The finished game has neither an interpreter nor a JIT: it is an ordinary
native executable. That is why speed is close to "native" rather than to an
emulator.

## Runtime

`runtime/src/` is a small layer that stands in for the phone:

* **core/guest.c** — the game's 32-bit address space, registers, flags, VFP.
  The guest sees the same addresses it saw on Android.
* **core/jni.c** — a fake `JNIEnv`. The game believes it talks to Java: file
  loading, strings, audio, vibration, video.
* **core/imports.c** — imports of `libc`, `libm`, `libGLESv1_CM`, `liblog`,
  `libz` and the rest. Paths like `/sdcard/gameloft/games/nova/...` are
  mapped onto the cache folder on disk.
* **gl/** — GLES 1.x on plain OpenGL 1.1 (a GDI context, no SDL/GLFW).
* **platform/** — the Win32 window, input, audio (Ogg Vorbis via
  stb_vorbis), video (Media Foundation) and diagnostics.
* **generated/** — the translation output. It is not in the repository: the
  builder creates it.

Intro and clips. On the phone the engine calls Java's `loadMovie(path)` and
waits for playback to finish. The `core/jni.c` shim remembers the request,
the main loop plays the file between frames and only then hands control back
to the game — which is why `GS_Intro` at startup behaves exactly as it does
on Android. The file is also looked for in `data\` next to `Nova.exe`, so
the game cache does not have to be renamed.

Window. By default this is an ordinary 1280×720 window; `F11` and
`Alt+Enter` toggle a borderless fullscreen. The values come from `[window]`
in `input.ini`, and the `RECOMP_RES`/`RECOMP_FULLSCREEN` variables override
them.

## Translator

`tools/armrecomp.py` parses the ELF, finds functions from `.symtab` and the
ARM/Thumb tables, disassembles with Capstone and emits C where every
instruction is an explicit operation on `CPU *cpu`.

Specifics:

* ARM and Thumb, including IT blocks and mode switching.
* VFPv2 (`vmov`, `vadd`, `vcmp`, ...) on top of `double d[16]`.
* Calls go through `host_call()` / a function table, so indirect calls and
  function pointers work as in the original.
* Unknown imports get a stub and are written to the log instead of crashing
  the game.
* NEON is not implemented: N.O.V.A. never executes it (only dead code from
  third-party libraries).

## Where the data comes from

The game requests paths like `/sdcard/gameloft/games/nova/entities.pack`.
The runtime looks for the file in:

1. `NOVA_DATA_DIR`, if the variable is set;
2. `data/` next to `Nova.exe` (this is what the builder sets up);
3. `gamefiles/nova` (the developer tree).

Writes always go to `%LOCALAPPDATA%\Nova\`; the cache is never modified.

## Input

The Android game only receives touches. Keyboard and mouse are translated
into touches in `platform/kbm.c`:

* the mouse moves the look "finger";
* WASD emulates the analog stick;
* LMB is fire;
* Esc sends the Android Back button (`notifyKeyPressed(4)`), which is
  exactly what the game uses for pause;
* Caps Lock enables drawing mode: up to three strokes are recorded and then
  replayed simultaneously as three touches.

## Diagnostics

If something goes wrong, look at `%LOCALAPPDATA%\Nova\nova.log`. Lines
tagged `[diag]` carry the frame, the time and the name of the engine
function currently running (symbols come from `.symtab` in `libnova.so`).
