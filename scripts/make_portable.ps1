# Portable Windows bundle for AdaptNote.
#
#   pwsh -File scripts\make_portable.ps1 [-QtDir "C:\Qt\6.8.3\msvc2022_64"]
#
# Produces dist\AdaptNote-<version>-windows-x64.zip containing:
#   AdaptNote.bat        launcher (data stays inside the folder)
#   bin\adaptnote.exe    the app (+ bundled MuPDF is static)
#   bin\*.dll            Qt 6 runtime, plugins and the MSVC runtime, staged by windeployqt
#   data\                created on first run (library.db / mirror / images)
#   samples\             test.pdf + test.epub
#
# Requirements on the build machine: CMake, Ninja (or MSVC), Qt 6.8+ MSVC kit
# (msvc2022_64; 6.5.x does not compile with VS18), MuPDF 1.21.1 built from
# platform\win32\mupdf.sln with /MD (pass -DMUPDF_ROOT to cmake) and Python 3 only
# for the sample EPUB. This is exactly what .github/workflows/windows-portable.yml does.

param(
    [string]$QtDir   = $env:QTDIR,
    [string]$Version = "0.22",
    [string]$BuildDir = "build-win",
    [string]$MuPdfRoot = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root

$name  = "AdaptNote-$Version-windows-x64"
$dist  = Join-Path $root "dist"
$stage = Join-Path $dist $name
$bin   = Join-Path $stage "bin"

Write-Host "== configure"
$cmakeArgs = @("-S", ".", "-B", $BuildDir, "-DCMAKE_BUILD_TYPE=Release")
if ($QtDir)     { $cmakeArgs += "-DCMAKE_PREFIX_PATH=$QtDir" }
if ($MuPdfRoot) { $cmakeArgs += "-DMUPDF_ROOT=$MuPdfRoot" }
& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

Write-Host "== build"
& cmake --build $BuildDir --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw "build failed" }

Write-Host "== stage"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path $bin, (Join-Path $stage "samples") -Force | Out-Null

$exe = Get-ChildItem -Path $BuildDir -Recurse -Filter adaptnote.exe |
       Where-Object { $_.FullName -notmatch "CMakeFiles" } | Select-Object -First 1
if (-not $exe) { throw "adaptnote.exe not found under $BuildDir" }
Copy-Item $exe.FullName (Join-Path $bin "adaptnote.exe")

$windeployqt = if ($QtDir) { Join-Path $QtDir "bin\windeployqt.exe" } else { "windeployqt.exe" }
Write-Host "== windeployqt ($windeployqt)"
& $windeployqt --release --no-translations --no-system-d3d-compiler --no-opengl-sw `
               --compiler-runtime (Join-Path $bin "adaptnote.exe")
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed" }

# MSVC C runtime: windeployqt --compiler-runtime usually copies them; make sure.
foreach ($dll in @("msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll")) {
    if (-not (Test-Path (Join-Path $bin $dll))) {
        $found = Get-ChildItem "C:\Windows\System32\$dll" -ErrorAction SilentlyContinue
        if ($found) { Copy-Item $found.FullName $bin }
    }
}

Write-Host "== samples"
Copy-Item "build\test.pdf"  (Join-Path $stage "samples") -ErrorAction SilentlyContinue
Copy-Item "build\test.epub" (Join-Path $stage "samples") -ErrorAction SilentlyContinue

Write-Host "== launcher"
$launcher = @"
@echo off
rem Portable launcher: keeps every byte of data inside this folder.
setlocal
set "HERE=%~dp0"
rem Qt ignores XDG_* on Windows, so point the app at the folder explicitly.
set "ADAPT_DATA_DIR=%HERE%data"
set "XDG_DATA_HOME=%HERE%data"
set "XDG_CONFIG_HOME=%HERE%data\config"
set "XDG_CACHE_HOME=%HERE%data\cache"
if not exist "%ADAPT_DATA_DIR%" mkdir "%ADAPT_DATA_DIR%"
if not exist "%XDG_CONFIG_HOME%" mkdir "%XDG_CONFIG_HOME%"
if not exist "%XDG_CACHE_HOME%" mkdir "%XDG_CACHE_HOME%"
start "" "%HERE%bin\adaptnote.exe" %*
"@
Set-Content -Path (Join-Path $stage "AdaptNote.bat") -Value $launcher -Encoding ASCII

Write-Host "== readme"
@"
AdaptNote（Windows 便携版）
==========================

运行：双击 AdaptNote.bat（或从命令行 AdaptNote.bat --doc 某本书.pdf）
数据：全部在 data\ 目录里（笔记库 data\library.db，图片 data\images\）；
      整个文件夹拷到 U 盘即可带走，删掉 data\ 恢复出厂状态。
示例：samples\test.pdf / samples\test.epub

提示：首次打开 PDF 若发现内嵌标注，会问一次是否读成笔记；
      快捷键 Ctrl+O 打开 · Ctrl+F 查找 · H/U/S/N 标注 · X 摘录图 · P 手写 · E 橡皮
             M 脑图 · R 朗读 · L 书库 · Ctrl+S 写入 PDF · Ctrl+Z 撤销
"@ | Set-Content -Path (Join-Path $stage "README.txt") -Encoding UTF8

Write-Host "== zip"
$zip = Join-Path $dist "$name.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path $stage -DestinationPath $zip -CompressionLevel Optimal
Get-Item $zip | ForEach-Object { "{0}  {1:N1} MB" -f $_.FullName, ($_.Length / 1MB) }
