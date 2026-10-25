@echo off
setlocal
cd /d "%~dp0port\run"
"%~dp0port\build\windows-x64-release\darkcloud.exe" --data "%~dp0port\run\data" --save "%~dp0port\run\dran-preview-save" --jump edit:0 --fast-load %*
