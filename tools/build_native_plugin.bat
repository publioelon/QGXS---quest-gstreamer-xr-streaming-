@echo off
cd /d "%~dp0..\receiver\native\GstQuestInitPlugin"
ndk-build
pause
