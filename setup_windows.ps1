# One-time ReflectAR setup for Windows: Python environment, model check, Unreal + Visual Studio checks,
# C++ build. Run it through setup.bat (double-click, or `setup.bat` in a terminal). Safe to run again.
# Written for Windows PowerShell 5.1, which every Windows 10/11 machine has.
param([switch]$NoBuild)

$ErrorActionPreference = 'Continue'
$Root = $PSScriptRoot
Set-Location $Root
$UEVersion = '5.8'
$Problems = 0

function Ok($m)   { Write-Host "  [ok] $m" -ForegroundColor Green }
function Warn($m) { Write-Host "  [!]  $m" -ForegroundColor Yellow }
function Fail($m) { Write-Host "  [x]  $m" -ForegroundColor Red; $script:Problems = 1 }
function Step($m) { Write-Host ""; Write-Host $m -ForegroundColor White }

# ---------------------------------------------------------------- Python
Step "1. Python environment (perception\.venv)"
$Py = $null
foreach ($v in '3.13', '3.12', '3.11') {
    if (Get-Command py -ErrorAction SilentlyContinue) {
        & py "-$v" -c "import sys" 2>$null
        if ($LASTEXITCODE -eq 0) { $Py = @('py', "-$v"); break }
    }
}
if (-not $Py -and (Get-Command python -ErrorAction SilentlyContinue)) {
    & python -c "import sys; sys.exit(0 if sys.version_info >= (3, 11) else 1)" 2>$null
    if ($LASTEXITCODE -eq 0) { $Py = @('python') }
}
if (-not $Py) {
    Fail "Python 3.11 or newer (64-bit) not found."
    Write-Host "    Install it with:  winget install Python.Python.3.12   (or from python.org; tick 'Add python.exe to PATH'),"
    Write-Host "    then open a NEW terminal and run setup.bat again."
    exit 1
}
$PyArgs = if ($Py.Count -gt 1) { $Py[1..($Py.Count - 1)] } else { @() }
Ok ("Using " + (& $Py[0] @PyArgs --version))

$Venv = Join-Path $Root 'perception\.venv'
$VPy = Join-Path $Venv 'Scripts\python.exe'
if (-not (Test-Path $VPy)) {
    & $Py[0] @PyArgs -m venv $Venv
    if ($LASTEXITCODE -ne 0) { Fail "Could not create $Venv"; exit 1 }
    Ok "Created perception\.venv"
}
# a venv made by uv has no pip: add it
& $VPy -m pip --version *> $null
if ($LASTEXITCODE -ne 0) { & $VPy -m ensurepip --upgrade *> $null }
& $VPy -m pip install -q --upgrade pip

# GPU runtime: CUDA wheels need an NVIDIA GPU; anything else uses DirectML (any DirectX 12 GPU)
$Gpus = (Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue | ForEach-Object { $_.Name }) -join ', '
$HasNvidia = $Gpus -match 'NVIDIA'
$Req = Join-Path $Root 'perception\requirements.txt'
$Lines = Get-Content $Req
if ($HasNvidia) {
    Ok "GPU: $Gpus -> CUDA (onnxruntime-gpu)"
} else {
    Ok "GPU: $Gpus -> DirectML (onnxruntime-directml)"
    $Lines = $Lines | ForEach-Object { if ($_ -match '^\s*onnxruntime-gpu') { 'onnxruntime-directml>=1.20' } else { $_ } }
}
$ReqLocal = Join-Path $Venv 'requirements-windows.txt'
$Lines | Set-Content -Encoding ASCII $ReqLocal
& $VPy -m pip install -q -r $ReqLocal
if ($LASTEXITCODE -eq 0) {
    Ok "Installed the Python packages"
} else {
    Warn "Full install failed; retrying without the RealSense package (only needed for a D415)"
    $Lines | Where-Object { $_ -notmatch 'pyrealsense2' } | Set-Content -Encoding ASCII $ReqLocal
    & $VPy -m pip install -q -r $ReqLocal
    if ($LASTEXITCODE -ne 0) { Fail "pip install failed (see above)"; exit 1 }
    Ok "Installed the Python packages (without RealSense: the webcam path works)"
}

Step "2. Model check"
Push-Location (Join-Path $Root 'perception')
& $VPy -c "import json; from backend import Backend; from blazepose import BlazePose; from handpose import HandPose; from depth import MonoDepth; b = Backend(json.load(open('calib/config.json'))['compute']); BlazePose(b); HandPose(b); MonoDepth(b); print('    ' + b.summary())"
if ($LASTEXITCODE -eq 0) { Ok "All models load" } else { Fail "The models did not load (see the error above)" }
Pop-Location

# ---------------------------------------------------------------- Visual Studio
Step "3. Visual Studio (needed to compile the Unreal project's C++)"
$VsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$Vs = $null
if (Test-Path $VsWhere) {
    $Vs = & $VsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json | ConvertFrom-Json | Select-Object -First 1
}
if (-not $Vs) {
    Fail "Visual Studio with C++ tools not found."
    Write-Host "    Install Visual Studio 2022 (17.14 or newer) or 2026 Community, with the workload"
    Write-Host "    'Game development with C++' (it includes the Windows SDK). Then run setup.bat again."
} else {
    $VsVer = [version]$Vs.installationVersion
    if ($VsVer.Major -lt 17 -or ($VsVer.Major -eq 17 -and $VsVer.Minor -lt 14)) {
        Fail ("Found " + $Vs.displayName + " " + $Vs.installationVersion + ": Unreal $UEVersion needs Visual Studio 2022 17.14+ or 2026. Update it in the Visual Studio Installer.")
    } else {
        Ok ($Vs.displayName + " " + $Vs.installationVersion)
    }
    $Sdks = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\Include" -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^10\.0\.\d+' }
    if ($Sdks) { Ok ("Windows SDK " + ($Sdks | Sort-Object Name | Select-Object -Last 1).Name) }
    else { Fail "No Windows 10/11 SDK found: add 'Windows 11 SDK' in the Visual Studio Installer." }
}

# ---------------------------------------------------------------- Unreal
Step "4. Unreal Engine $UEVersion"
$UERoot = $env:UE_ROOT
if (-not $UERoot) {
    $Key = "HKLM:\SOFTWARE\EpicGames\Unreal Engine\$UEVersion"
    if (Test-Path $Key) { $UERoot = (Get-ItemProperty $Key).InstalledDirectory }
}
if (-not $UERoot) { $UERoot = Join-Path $env:ProgramFiles "Epic Games\UE_$UEVersion" }
$BuildBat = Join-Path $UERoot 'Engine\Build\BatchFiles\Build.bat'
if (-not (Test-Path $BuildBat)) {
    Fail "Unreal Engine $UEVersion not found (looked in $UERoot)."
    Write-Host "    Install the Epic Games Launcher (epicgames.com, or: winget install EpicGames.EpicGamesLauncher), sign in,"
    Write-Host "    then Unreal Engine > Library > + > $UEVersion. Installed elsewhere? In PowerShell:  `$env:UE_ROOT='D:\UE_$UEVersion'; .\setup_windows.ps1"
} else {
    Ok "Found Unreal Engine at $UERoot"
    if (-not $NoBuild -and $Problems -eq 0) {
        Step "5. Compiling the ReflectAR editor module (a few minutes the first time)"
        $Log = Join-Path $Root 'unreal\build.log'
        & $BuildBat ReflectAREditor Win64 Development "-Project=$Root\unreal\ReflectAR.uproject" -WaitMutex *> $Log
        if ($LASTEXITCODE -eq 0) { Ok "Build succeeded" }
        else {
            Select-String -Path $Log -Pattern 'error' | Select-Object -First 15 | ForEach-Object { Write-Host "    $($_.Line)" }
            Fail "Build failed: full log in unreal\build.log"
        }
    } elseif (-not $NoBuild) {
        Warn "Skipping the build until the problems above are fixed"
    }
}

# ---------------------------------------------------------------- done
Step "Summary"
if ($Problems -eq 0) {
    Ok "Everything is set up."
    Write-Host ""
    Write-Host "    Next:"
    Write-Host "      1. run_perception.bat          (allow desktop apps to use the camera if Windows asks)"
    Write-Host "      2. double-click unreal\ReflectAR.uproject, wait for shaders to compile, press Play"
    Write-Host "      3. stand 1-3 m from the screen"
    Write-Host "    No camera handy?  cd perception; .venv\Scripts\python fake_server.py   instead of step 1."
} else {
    Fail "Some steps need attention (marked [x] above). Fix them and run setup.bat again; see SETUP.md > Troubleshooting."
    exit 1
}
