@echo off
setlocal

cd /d "%~dp0"

where conda >nul 2>nul
if %ERRORLEVEL% EQU 0 (
    call conda activate gstwebrtc
    python launcher_gui.py
    exit /b %ERRORLEVEL%
)

where python >nul 2>nul
if %ERRORLEVEL% EQU 0 (
    python launcher_gui.py
    exit /b %ERRORLEVEL%
)

echo Python was not found.
echo Install Miniconda, create the gstwebrtc environment, and try again.
pause
exit /b 1