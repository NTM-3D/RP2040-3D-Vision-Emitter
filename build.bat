@echo off
setlocal

for %%I in ("%~dp0.") do set "SCRIPT_DIR=%%~fI"
for %%I in ("%SCRIPT_DIR%\..") do set "WORKSPACE_DIR=%%~fI"

set "PICO_SDK_PATH=%WORKSPACE_DIR%\pico-sdk"
set "TOOLCHAIN_BIN=%USERPROFILE%\.platformio\packages\toolchain-gccarmnoneeabi\bin"
set "PY_SCRIPTS=%APPDATA%\Python\Python313\Scripts"
set "BUILD_DIR=%SCRIPT_DIR%\build"

if /I "%~1"=="clean" goto :clean_build
if /I "%~1"=="-clean" goto :clean_build
goto :after_clean

:clean_build
    if exist "%BUILD_DIR%" (
        echo Cleaning "%BUILD_DIR%" ...
        attrib -r "%BUILD_DIR%\*" /s /d >nul 2>nul
        rmdir /s /q "%BUILD_DIR%"
        if exist "%BUILD_DIR%" (
            echo Failed to remove "%BUILD_DIR%".
            echo Make sure no terminal or Explorer window is holding files open there.
            exit /b 1
        )
    )

:after_clean

if not exist "%PICO_SDK_PATH%\pico_sdk_init.cmake" (
    echo pico-sdk not found at "%PICO_SDK_PATH%"
    exit /b 1
)

if not exist "%TOOLCHAIN_BIN%\arm-none-eabi-gcc.exe" (
    echo ARM GCC toolchain not found at "%TOOLCHAIN_BIN%"
    exit /b 1
)

set "PATH=%PY_SCRIPTS%;%TOOLCHAIN_BIN%;%PATH%"

cmake -S "%SCRIPT_DIR%" -B "%BUILD_DIR%" -G Ninja
if errorlevel 1 exit /b 1

cmake --build "%BUILD_DIR%" -j
if errorlevel 1 exit /b 1

echo.
echo Built UF2:
echo %BUILD_DIR%\RP2040_3D_Vision_Emitter.uf2
echo.
echo Usage:
echo   build.bat        ^(incremental build^)
echo   build.bat -clean  ^(delete build dir then full rebuild^)

endlocal