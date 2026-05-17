@echo off
set "SRC=%~dp0..\receiver\native\GstQuestInitPlugin\libs\arm64-v8a"
set "DST=%~dp0..\receiver\unity\QuestGStreamerReceiver\Assets\Plugins\Android\libs\arm64-v8a"
mkdir "%DST%" 2>nul
copy /Y "%SRC%\*.so" "%DST%\"
pause
