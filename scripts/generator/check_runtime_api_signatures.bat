@echo off
setlocal
call "%~dp0..\lib\repo-root.bat"
cd /d "%REPO_ROOT%\src\generator"
python check_runtime_api_signatures.py ^
  --profile mono45 ^
  --repo-root "%REPO_ROOT%" ^
  %*
exit /b %ERRORLEVEL%
