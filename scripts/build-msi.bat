@echo off
setlocal enabledelayedexpansion

set PROJECT_DIR=%~dp0..
set WINDOWS_DIR=%PROJECT_DIR%\desktop\windows
set BUILD_DIR=%WINDOWS_DIR%\build-msi-x64

:: Read version from the single-source-of-truth VERSION file
set /p VERSION=<"%PROJECT_DIR%\VERSION"

if "%VERSION%"=="" (
    echo ERROR: Could not read version from %PROJECT_DIR%\VERSION
    exit /b 1
)
echo Building VoiceStick v%VERSION% MSI installer...

:: Initialize VS build environment (cmake, ninja, cl, rc, etc.)
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

:: Step 1: CMake configure + build (RelWithDebInfo)
echo.
echo [1/4] CMake RelWithDebInfo build...
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

:: Step 2: Sign exe files (signtool from Windows SDK, PATH, or local signing folder)
:: Certificate thumbprint: set env SIGNING_SHA1, or create scripts\.signing_sha1
:: with one line containing the certificate thumbprint (SHA1).
if not defined SIGNING_SHA1 (
    if exist "%~dp0.signing_sha1" (
        for /f "usebackq delims=" %%i in ("%~dp0.signing_sha1") do set "SIGNING_SHA1=%%i"
    )
)
if not defined SIGNING_SHA1 (
    for /f "usebackq delims=" %%i in (`powershell -NoProfile -Command "$certs = @(Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert -ErrorAction SilentlyContinue) + @(Get-ChildItem Cert:\LocalMachine\My -CodeSigningCert -ErrorAction SilentlyContinue); $valid = @($certs | Where-Object { $_.NotAfter -gt (Get-Date) -and $_.HasPrivateKey }); if ($valid.Count -eq 1) { $valid[0].Thumbprint }"`) do set "SIGNING_SHA1=%%i"
)
if defined SIGNING_SHA1 set "SIGNING_SHA1=%SIGNING_SHA1: =%"

if defined SIGNTOOL_PATH (
    set SIGNTOOL=%SIGNTOOL_PATH%
) else (
    set SIGNTOOL=signtool
)
where "%SIGNTOOL%" >nul 2>&1
if errorlevel 1 (
    if exist "%ProgramFiles(x86)%\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe" (
        set "SIGNTOOL=%ProgramFiles(x86)%\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe"
    ) else if exist "%ProgramFiles(x86)%\Windows Kits\10\App Certification Kit\signtool.exe" (
        set "SIGNTOOL=%ProgramFiles(x86)%\Windows Kits\10\App Certification Kit\signtool.exe"
    ) else if exist "D:\Workspace\???\signtool.exe" (
        set SIGNTOOL=D:\Workspace\???\signtool.exe
    )
)
if not exist "%SIGNTOOL%" (
    where "%SIGNTOOL%" >nul 2>&1
    if errorlevel 1 (
        echo ERROR: signtool.exe not found. Set SIGNTOOL_PATH to the full signtool.exe path.
        exit /b 1
    )
)

echo.
echo [2/4] Signing binaries...
if defined SIGNING_SHA1 (
    set SIGN_ARGS=/v /fd sha256 /sha1 %SIGNING_SHA1% /tr http://timestamp.digicert.com /td sha256
) else (
    set SIGN_ARGS=/v /fd sha256 /a /uw /tr http://timestamp.digicert.com /td sha256
)
"%SIGNTOOL%" sign %SIGN_ARGS% "%BUILD_DIR%\VoiceStick.exe"
if errorlevel 1 (
    echo ERROR: Signing VoiceStick.exe failed.
    exit /b 1
)
powershell -NoProfile -Command "$sig = Get-AuthenticodeSignature -FilePath '%BUILD_DIR%\VoiceStick.exe'; if ($sig.SignerCertificate) { exit 0 }; exit 1"
if errorlevel 1 (
    echo ERROR: VoiceStick.exe is not signed.
    exit /b 1
)
"%SIGNTOOL%" sign %SIGN_ARGS% "%BUILD_DIR%\WinSparkle.dll"
if errorlevel 1 (
    echo ERROR: Signing WinSparkle.dll failed.
    exit /b 1
)
powershell -NoProfile -Command "$sig = Get-AuthenticodeSignature -FilePath '%BUILD_DIR%\WinSparkle.dll'; if ($sig.SignerCertificate) { exit 0 }; exit 1"
if errorlevel 1 (
    echo ERROR: WinSparkle.dll is not signed.
    exit /b 1
)

:: Step 3: Build MSI with WiX
echo.
echo [3/4] Building MSI with WiX...
:: Optional: inject a real config (with secrets) via VOICESTICK_CONFIG_TEMPLATE to override the placeholder.
:: Used to distribute a pre-configured MSI to testers; secrets are injected only at local build time, never committed.
:: On first launch the exe-adjacent config.template.toml is copied to %APPDATA%\VoiceStick\config.toml if absent.
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

:: Step 4: Sign MSI installer
echo.
echo [4/4] Signing MSI...
"%SIGNTOOL%" sign %SIGN_ARGS% "%BUILD_DIR%\VoiceStick_%VERSION%.msi"
if errorlevel 1 (
    echo ERROR: Signing MSI failed.
    exit /b 1
)
powershell -NoProfile -Command "$sig = Get-AuthenticodeSignature -FilePath '%BUILD_DIR%\VoiceStick_%VERSION%.msi'; if ($sig.SignerCertificate) { exit 0 }; exit 1"
if errorlevel 1 (
    echo ERROR: MSI is not signed.
    exit /b 1
)

echo.
echo Success: %BUILD_DIR%\VoiceStick_%VERSION%.msi
