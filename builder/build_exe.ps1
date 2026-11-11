<#
Builds NovaPortBuilder.exe from builder/nova_build.py.

  powershell -ExecutionPolicy Bypass -File builder\build_exe.ps1

Result: builder\dist\NovaPortBuilder.exe
#>

param(
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

python -c "import PyInstaller" 2>$null
if ($LASTEXITCODE -ne 0) {
    Write-Host "PyInstaller is missing, installing..."
    python -m pip install --upgrade pyinstaller
    if ($LASTEXITCODE -ne 0) { throw "could not install PyInstaller" }
}

$pyArgs = @("--noconfirm")
if ($Clean) { $pyArgs += "--clean" }
$pyArgs += "nova_build.spec"

python -m PyInstaller @pyArgs
if ($LASTEXITCODE -ne 0) { throw "PyInstaller failed" }

$exe = Join-Path $PSScriptRoot "dist\NovaPortBuilder.exe"
if (-not (Test-Path $exe)) { throw "NovaPortBuilder.exe was not produced" }

$size = "{0:N1} MB" -f ((Get-Item $exe).Length / 1MB)
Write-Host ""
Write-Host "Done: $exe ($size)"
