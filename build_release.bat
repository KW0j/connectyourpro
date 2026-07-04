@echo off
echo === joycon2cpp build script ===
call "X:\VS2022BuildTools\VC\Auxiliary\Build\vcvars64.bat"

set CMAKE=C:\Program Files\CMake\bin\cmake.exe
set BUILD_DIR=%~dp0build

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

echo.
echo --- CMake Configure ---
"%CMAKE%" "%~dp0testapp" -G "Ninja" -B "%BUILD_DIR%" -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 (
    echo Configure failed!
    exit /b 1
)

echo.
echo --- CMake Build ---
"%CMAKE%" --build "%BUILD_DIR%"
if errorlevel 1 (
    echo Build failed!
    exit /b 1
)

echo.
echo === Build successful! ===
echo Output: %BUILD_DIR%\testapp.exe
