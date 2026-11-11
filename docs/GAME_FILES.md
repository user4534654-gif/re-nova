# Game files

The repository contains no game files. To build, you need your own legally
obtained APK and cache.

## APK

Version **1.1.4** is required, package
`com.gameloft.android.TBFV.GloftNOHP.ML`.

The builder takes only this out of the APK:

```
lib/armeabi-v7a/libnova.so
```

The rest of the APK (Java code, resources) is not used.

The known original:

| | |
|---|---|
| size | 7,222,968 bytes |
| sha256 | `a04448706c1e2b62fa880845ac5f36c09c3f0ab24a26c3ac309aab25b1f57e43` |

If the hash differs the builder still tries to build, but behaviour is not
guaranteed: the game version must be 1.1.4.

## Cache

On a phone this is usually:

```
/Android/data/com.gameloft.android.TBFV.GloftNOHP.ML/files/gameloft/games/nova/
```

or (older firmware):

```
/sdcard/gameloft/games/nova/
```

It must contain:

```
*.pack              level, menu and entity data
sounds/             sounds (*.glsnd or *.ogg)
NOVA.mp4            video
NOVAIN.mp4          video
*.tga               textures
oconf.bar           configuration
```

The builder accepts either the `nova` folder itself or one of its parents
(`gameloft/games/nova`, `.../files`, the cache root) — it finds the folder
with `*.pack` and `sounds/` on its own.

Housekeeping `.svn` and `.git` entries are skipped while copying.

## What the port writes to disk

Only inside `%LOCALAPPDATA%\Nova\`:

* game saves;
* `input.ini` — input and window settings (`[window]`: size, fullscreen,
  video, the creator's link);
* `nova.log` — the log.

The cache in `data\` is left untouched, so it can be kept read-only.
