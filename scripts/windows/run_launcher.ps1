$ErrorActionPreference = "Stop"

$RepoRoot = Resolve-Path "$PSScriptRoot\..\.."
$Launcher = Join-Path $RepoRoot "launcher\launcher_gui.py"

if (!(Test-Path $Launcher)) {
    throw "Launcher not found: $Launcher"
}

python $Launcher
