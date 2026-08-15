@echo off
setlocal enabledelayedexpansion

:: Unsigned MSI build script. Based on build-msi.bat, with all signtool
:: signing and signature verification steps removed. Use this only when the
:: local machine has no code-signing certificate; for official releases use
:: build-msi.bat on the signing machine.

set PROJECT_DIR=%~dp0..
set WINDOWS_DIR=%PROJECT_DIR%\desktop\windows
set BUILD_DIR=%WINDOWS_DIR%\build-msi-x64

set /p VERSION=<"%PROJECT_DIR%\VERSION"

if "%VERSION%"=="" (
    echo ERROR: Could not read version from %PROJECT_DIR%\VERSION
    exit /b 1
)
echo Building VoiceStick v%VERSION% MSI installer (UNSIGNED)...

set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist %VSWHERE% (
    for /f "delims=" %%i in ('%VSWHERE% -latest -property installationPath') do set VS_PATH=%%i
)
if not exist "%VS_PATH%\VC\Auxiliary\Build\vcvarsall.bat" (
    if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" (
        set "VS_PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools"
    )
)
if not exist "%VS_PATH%\VC\Auxiliary\Build\vcvarsall.bat" (
    echo ERROR: Could not find vcvarsall.bat. Is Visual Studio installed?
    exit /b 1
)
call "%VS_PATH%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1

:: Prepend the C++/WinRT projection headers (desktop\windows\generated_winrt) to INCLUDE.
:: Same reason as build_win.bat: the Windows SDK's Include\<ver>\winrt folder only ships
:: legacy WRL-style headers and lacks winrt/base.h. The headers are generated once by
:: build_win.bat and cached under generated_winrt (gitignored, survives clean).
:: If missing, build would fail with C1083 on winrt/base.h.
set "GEN_WINRT_DIR=%WINDOWS_DIR%\generated_winrt"
if not exist "%GEN_WINRT_DIR%\winrt\base.h" (
    echo WARNING: %GEN_WINRT_DIR%\winrt\base.h not found.
    echo          Run build_win.bat first to generate C++/WinRT projection headers,
    echo          or this build will fail with C1083 on winrt/base.h.
)
set "INCLUDE=%GEN_WINRT_DIR%;%INCLUDE%"

echo.
echo [1/2] CMake RelWithDebInfo build...
cmake -S "%WINDOWS_DIR%" -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
if errorlevel 1 (
    echo ERROR: CMake configure failed.
    exit /b 1
)

cmake --build "%BUILD_DIR%" --target clean
if errorlevel 1 (
    echo ERROR: CMake clean failed.
    exit /b 1
)

cmake --build "%BUILD_DIR%" --config RelWithDebInfo
if errorlevel 1 (
    echo ERROR: CMake build failed.
    exit /b 1
)

if not exist "%BUILD_DIR%\VoiceStick.exe" (
    echo ERROR: VoiceStick.exe not found in build directory.
    exit /b 1
)
if not exist "%BUILD_DIR%\WinSparkle.dll" (
    echo ERROR: WinSparkle.dll not found in build directory.
    exit /b 1
)

echo.
echo [2/2] Building MSI with WiX (unsigned)...
:: Optional: inject a real config (with secrets) via VOICESTICK_CONFIG_TEMPLATE to override the placeholder.
:: Used to distribute a pre-configured MSI to testers; secrets are injected only at local build time, never committed.
:: See Doc/Plan/windows-msi-config-template-seed.md for details.
if defined VOICESTICK_CONFIG_TEMPLATE (
    if exist "%VOICESTICK_CONFIG_TEMPLATE%" (
        echo Injecting config template from: %VOICESTICK_CONFIG_TEMPLATE%
        copy /Y "%VOICESTICK_CONFIG_TEMPLATE%" "%BUILD_DIR%\config.template.toml" >nul
        if errorlevel 1 (
            echo ERROR: Failed to copy VOICESTICK_CONFIG_TEMPLATE to build dir.
            exit /b 1
        )
    ) else (
        echo WARNING: VOICESTICK_CONFIG_TEMPLATE set but not found: %VOICESTICK_CONFIG_TEMPLATE%
        echo          Falling back to placeholder template from resources\config.template.toml
    )
)
if not defined WIX_PATH (
    if exist "%USERPROFILE%\.dotnet\tools\wix.exe" (
        set "WIX_PATH=%USERPROFILE%\.dotnet\tools\wix.exe"
    ) else (
        set WIX_PATH=C:\Program Files\WiX Toolset v6.0\bin\wix.exe
    )
)
if not exist "%WIX_PATH%" (
    echo ERROR: WiX not found at %WIX_PATH%
    exit /b 1
)
"%WIX_PATH%" build "%WINDOWS_DIR%\installer\VoiceStick.wxs" ^
    "%WINDOWS_DIR%\installer\zh-CN.wxl" ^
    -arch x64 ^
    -culture zh-CN ^
    -ext WixToolset.UI.wixext ^
    -ext WixToolset.Util.wixext ^
    -d ProductVersion=%VERSION% ^
    -d BuildDir=%BUILD_DIR% ^
    -d ProjectDir=%PROJECT_DIR% ^
    -o "%BUILD_DIR%\VoiceStick_%VERSION%.msi"
if errorlevel 1 (
    echo ERROR: WiX build failed.
    exit /b 1
)

echo.
echo Success (unsigned): %BUILD_DIR%\VoiceStick_%VERSION%.msi
