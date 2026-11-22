@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Reflections-Deployment.ps1" -Action Restore
set "deploy_result=%errorlevel%"
pause
exit /b %deploy_result%
