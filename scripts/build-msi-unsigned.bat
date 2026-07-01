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
