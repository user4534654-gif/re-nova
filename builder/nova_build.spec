# -*- mode: python ; coding: utf-8 -*-
"""PyInstaller spec for NovaPortBuilder.exe.

The EXE opens the graphic interface by default (a small local web page);
`--cli` keeps the old console behaviour.  Running it from cmd.exe still
attaches to that console, so the console mode stays usable.
"""

from pathlib import Path
from PyInstaller.utils.hooks import collect_submodules

REPO = Path(SPECPATH).resolve().parent

hidden = collect_submodules("elftools") + collect_submodules("capstone")
hidden += ["nova_gui"]

datas = [
    (str(REPO / "tools"), "tools"),
    (str(REPO / "runtime"), "runtime"),
    (str(REPO / "builder" / "web"), "builder/web"),
]

a = Analysis(
    [str(REPO / "builder" / "nova_build.py")],
    pathex=[str(REPO)],
    binaries=[],
    datas=datas,
    hiddenimports=hidden,
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=["unittest", "pydoc"],
    noarchive=False,
)
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name="NovaPortBuilder",
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=False,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
)
