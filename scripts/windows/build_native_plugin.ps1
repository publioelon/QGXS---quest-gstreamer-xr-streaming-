param(
    [string]$Configuration = "Release",
    [string]$Platform = "x64",
    [string]$GStreamerRoot = "C:\Program Files\gstreamer\1.0\msvc_x86_64"
)

$ErrorActionPreference = "Stop"

$RepoRoot = Resolve-Path "$PSScriptRoot\..\.."
$NativeProject = Join-Path $RepoRoot "receiver\windows\native\GStreamerUnityPlugin\GStreamerUnityPlugin.vcxproj"
$UnityPluginDir = Join-Path $RepoRoot "receiver\windows\unity\GStreamerUnity\Assets\Plugins\GStreamerUnity\x64"

if (!(Test-Path $NativeProject)) {
    throw "Native plugin project not found: $NativeProject"
}

if (!(Test-Path $GStreamerRoot)) {
    throw "GStreamer root not found: $GStreamerRoot"
}

$env:GSTREAMER_1_0_ROOT_MSVC_X86_64 = $GStreamerRoot
$env:PATH = "$GStreamerRoot\bin;$env:PATH"

$VsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"

if (!(Test-Path $VsWhere)) {
    throw "vswhere.exe not found. Install Visual Studio 2022 with C++ build tools."
}

$MSBuild = & $VsWhere -latest -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1

if ([string]::IsNullOrWhiteSpace($MSBuild) -or !(Test-Path $MSBuild)) {
    throw "MSBuild not found. Install Visual Studio 2022 with C++ build tools."
}

Write-Host "Building native plugin..." -ForegroundColor Cyan
Write-Host "Project: $NativeProject"
Write-Host "GStreamer: $GStreamerRoot"
Write-Host "Configuration: $Configuration"
Write-Host "Platform: $Platform"

& $MSBuild $NativeProject /m /p:Configuration=$Configuration /p:Platform=$Platform

if ($LASTEXITCODE -ne 0) {
    throw "MSBuild failed with exit code $LASTEXITCODE"
}

$Dll = Get-ChildItem (Split-Path $NativeProject) -Recurse -Filter "GStreamerUnityPlugin.dll" |
    Where-Object { $_.FullName -notmatch "\\Assets\\Plugins\\" } |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1

if ($Dll -eq $null) {
    throw "Could not find built GStreamerUnityPlugin.dll"
}

New-Item -ItemType Directory -Force -Path $UnityPluginDir | Out-Null
Copy-Item $Dll.FullName (Join-Path $UnityPluginDir "GStreamerUnityPlugin.dll") -Force

Write-Host "Native plugin installed into Unity project:" -ForegroundColor Green
Write-Host (Join-Path $UnityPluginDir "GStreamerUnityPlugin.dll") -ForegroundColor Green
