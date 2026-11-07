# Portable Windows bundle for AdaptNote.
#
#   pwsh -File scripts\make_portable.ps1 [-QtDir "C:\Qt\6.8.3\msvc2022_64"]
#
# Produces dist\AdaptNote-<version>-windows-x64.zip containing:
#   AdaptNote.bat        optional launcher (data stays inside the folder)
#   portable.flag       lets bin\adaptnote.exe resolve ..\data\ by itself
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
    [string]$MuPdfRoot = "",
    # Bundle a local tesseract (UB-Mannheim build + tessdata_fast eng/chi_sim) and a
    # trimmed ECDICT into the portable folder. Downloads ~270 MB during packaging.
    [bool]$WithExtras = $true
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
New-Item -ItemType Directory -Path $bin, (Join-Path $stage "samples"), (Join-Path $stage "data") -Force | Out-Null
Set-Content -Path (Join-Path $stage "portable.flag") -Value "AdaptNote portable bundle v1" -Encoding ascii

$exe = Get-ChildItem -Path $BuildDir -Recurse -Filter adaptnote.exe |
       Where-Object { $_.FullName -notmatch "CMakeFiles" } | Select-Object -First 1
if (-not $exe) { throw "adaptnote.exe not found under $BuildDir" }
Copy-Item $exe.FullName (Join-Path $bin "adaptnote.exe")

$windeployqt = if ($QtDir) { Join-Path $QtDir "bin\windeployqt.exe" } else { "windeployqt.exe" }
Write-Host "== windeployqt ($windeployqt)"
& $windeployqt --release --no-translations --no-system-d3d-compiler --no-opengl-sw `
               (Join-Path $bin "adaptnote.exe")
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed" }

# windeployqt only ships the redist *installer* with --compiler-runtime; we copy the DLLs
# ourselves below, so prune the installer and the D3D shader compiler payload (~34 MB).
foreach ($junk in @("vc_redist.x64.exe", "dxcompiler.dll", "dxil.dll")) {
    $p = Join-Path $bin $junk
    if (Test-Path $p) { Remove-Item $p -Force; Write-Host "   pruned $junk" }
}

# MSVC C runtime: windeployqt --compiler-runtime usually copies them; make sure.
foreach ($dll in @("msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll")) {
    if (-not (Test-Path (Join-Path $bin $dll))) {
        $found = Get-ChildItem "C:\Windows\System32\$dll" -ErrorAction SilentlyContinue
        if ($found) { Copy-Item $found.FullName $bin }
    }
}

Write-Host "== samples"
$samples = Join-Path $stage "samples"
$copied = 0
foreach ($sample in @("test.pdf", "test.epub")) {
    $src = Join-Path $BuildDir $sample
    if (-not (Test-Path $src)) { $src = Join-Path "build" $sample }   # fallback for old layouts
    if (Test-Path $src) { Copy-Item $src $samples; $copied++ }
    else { Write-Host "   (missing $sample in $BuildDir)" }
}
if ($copied -eq 0) { Write-Host "   WARNING: no sample documents staged" }

# ------------------------------------------------------------------ bundled extras
# Layout inside the bundle:
#   ocr\tesseract.exe + dlls + ocr\tessdata\{eng,chi_sim}.traineddata
#   data\ecdict.db                     (word -> phonetic/translation/definition)
# The app finds the portable copy before any machine install (see ocr.cpp).
if ($WithExtras) {
    $downloads = Join-Path $dist "downloads"
    New-Item -ItemType Directory -Path $downloads -Force | Out-Null

    function Get-FileFromWeb([string]$Url, [string]$Dest) {
        if (Test-Path $Dest) { return $true }
        Write-Host "   downloading $(Split-Path -Leaf $Dest)"
        & curl.exe -sSL --retry 3 -o $Dest $Url
        return ($LASTEXITCODE -eq 0 -and (Test-Path $Dest))
    }

    Write-Host "== bundled OCR (tesseract 5.4 + eng/chi_sim)"
    $ocrStage = Join-Path $stage "ocr"
    try {
        $setup = Join-Path $downloads "tesseract-ocr-w64-setup-5.4.0.20240606.exe"
        if (-not (Get-FileFromWeb "https://github.com/UB-Mannheim/tesseract/releases/download/v5.4.0.20240606/tesseract-ocr-w64-setup-5.4.0.20240606.exe" $setup)) {
            throw "tesseract setup download failed"
        }
        $extract = Join-Path $dist "tesseract-extract"
        if (Test-Path $extract) { Remove-Item $extract -Recurse -Force }
        New-Item -ItemType Directory -Path $extract -Force | Out-Null
        & 7z x $setup "-o$extract" -y | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "7z extraction failed (is 7-Zip installed?)" }
        $exe = Get-ChildItem -Path $extract -Recurse -Filter tesseract.exe |
               Select-Object -First 1
        if (-not $exe) { throw "tesseract.exe not found inside the installer" }
        New-Item -ItemType Directory -Path $ocrStage -Force | Out-Null
        Copy-Item $exe.FullName (Join-Path $ocrStage "tesseract.exe") -Force
        foreach ($dll in (Get-ChildItem -Path $exe.DirectoryName -Filter *.dll)) {
            Copy-Item $dll.FullName $ocrStage -Force
        }
        # Prune everything the CLI does not need: installer tessdata (legacy+LSTM, big),
        # docs and the D3D runtime some builds ship.
        foreach ($junk in @("d3dcompiler_47.dll", "dxcompiler.dll", "dxil.dll", "docs")) {
            $p = Join-Path $ocrStage $junk
            if (Test-Path $p) { Remove-Item $p -Recurse -Force }
        }
        $tessdata = Join-Path $ocrStage "tessdata"
        New-Item -ItemType Directory -Path $tessdata -Force | Out-Null
        foreach ($lang in @("eng", "chi_sim")) {
            $pack = Join-Path $downloads "$lang.traineddata"
            if (-not (Get-FileFromWeb "https://github.com/tesseract-ocr/tessdata_fast/raw/main/$lang.traineddata" $pack)) {
                throw "$lang.traineddata download failed"
            }
            Copy-Item $pack (Join-Path $tessdata "$lang.traineddata") -Force
        }
        $ocrExe = Join-Path $ocrStage "tesseract.exe"
        & $ocrExe --version | Select-Object -First 1 | ForEach-Object { Write-Host "   $_" }
    } catch {
        Write-Warning "bundled OCR skipped: $($_.Exception.Message)"
        if (Test-Path $ocrStage) { Remove-Item $ocrStage -Recurse -Force }
    }

    Write-Host "== bundled dictionary (ECDICT, top entries)"
    try {
        $ecdictZip = Join-Path $downloads "ecdict-sqlite-28.zip"
        if (-not (Get-FileFromWeb "https://github.com/skywind3000/ECDICT/releases/download/1.0.28/ecdict-sqlite-28.zip" $ecdictZip)) {
            throw "ecdict download failed"
        }
        New-Item -ItemType Directory -Path (Join-Path $stage "data") -Force | Out-Null
        $ecdictOut = Join-Path $stage "data\ecdict.db"
        & python $root\scripts\make_ecdict.py $ecdictZip $ecdictOut
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path $ecdictOut)) {
            throw "ecdict.db build failed"
        }
        Get-Item $ecdictOut | ForEach-Object { Write-Host ("   {0:N1} MB" -f ($_.Length / 1MB)) }
        # The unpacked full dictionary is ~780 MB of scratch; drop it once done.
        $scratch = Join-Path $dist "ecdict-extract"
        if (Test-Path $scratch) { Remove-Item $scratch -Recurse -Force }
    } catch {
        Write-Warning "bundled dictionary skipped: $($_.Exception.Message)"
        if (Test-Path (Join-Path $stage "data\ecdict.db")) {
            Remove-Item (Join-Path $stage "data\ecdict.db") -Force
        }
    }
}

Write-Host "== verify the staged tree"
# Fail here rather than shipping a zip that cannot start: a missing platform plugin makes
# Qt abort with "could not find or load the Qt platform plugin windows".
foreach ($need in @("bin\adaptnote.exe", "bin\Qt6Core.dll", "bin\Qt6Gui.dll",
                    "bin\Qt6Widgets.dll", "bin\platforms\qwindows.dll", "portable.flag")) {
    if (-not (Test-Path (Join-Path $stage $need))) { throw "staged package is missing $need" }
}
$kept = @("adaptnote.exe","Qt6Core.dll","Qt6Gui.dll","Qt6Widgets.dll","Qt6Network.dll",
          "Qt6Sql.dll","Qt6Svg.dll","msvcp140.dll","vcruntime140.dll","vcruntime140_1.dll")
foreach ($f in (Get-ChildItem $bin -File | Where-Object { $_.Extension -eq ".dll" -or $_.Extension -eq ".exe" })) {
    if ($kept -notcontains $f.Name) { Write-Host "   note: extra file in bin\$($f.Name)" }
}

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

运行：双击 bin\adaptnote.exe 或 AdaptNote.bat（可带 --doc 某本书.pdf）
      程序已配置为直接运行时将数据保存在本目录 data\ 中。
数据：全部在 data\ 目录里（笔记库 data\library.db，图片 data\images\）；
      整个文件夹拷到 U 盘即可带走，删掉 data\ 恢复出厂状态。
示例：samples\test.pdf / samples\test.epub
OCR：包内自带 tesseract（ocr\ 目录，eng+chi_sim）；语言包可在「更多 → OCR 设置…」改。
查词：包内自带 ECDICT 精简版（data\ecdict.db，按词频取常用词条）；
      想要全量词典时下载 ecdict-sqlite-28.zip 解压后替换同名文件即可。

提示：首次打开 PDF 若发现内嵌标注，会问一次是否读成笔记；
      快捷键 Ctrl+O 打开 · Ctrl+F 查找 · H/U/S/N 标注 · X 摘录图 · P 手写 · E 橡皮
             M 脑图 · R 朗读 · L 书库 · Ctrl+S 写入 PDF · Ctrl+Z 撤销
"@ | Set-Content -Path (Join-Path $stage "README.txt") -Encoding UTF8

Write-Host "== zip"
$zip = Join-Path $dist "$name.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path $stage -DestinationPath $zip -CompressionLevel Optimal
Get-Item $zip | ForEach-Object { "{0}  {1:N1} MB" -f $_.FullName, ($_.Length / 1MB) }
