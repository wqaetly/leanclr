@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..\..\..") do set "REPO_ROOT=%%~fI"

set "EXE=%REPO_ROOT%\out\dotnet\ExportExtern\Debug\net8.0\ExportExtern.dll"
if not exist "%EXE%" (
    dotnet build "%SCRIPT_DIR%ExportExtern.csproj" -c Debug
    if errorlevel 1 exit /b %ERRORLEVEL%
)

if not exist "%EXE%" (
    echo Executable not found: "%EXE%"
    exit /b 1
)

set "WIN_LIBRARIES_DIR=%REPO_ROOT%\src\libraries\mono-4.5"
if not exist "%WIN_LIBRARIES_DIR%" (
    echo Libraries directory not found: "%WIN_LIBRARIES_DIR%"
    exit /b 1
)

for %%f in (mscorlib System System.Core) do (
    dotnet "%EXE%" "%WIN_LIBRARIES_DIR%\%%f.dll" all "%WIN_LIBRARIES_DIR%\%%f_externs.txt"
    if errorlevel 1 exit /b %ERRORLEVEL%
)
