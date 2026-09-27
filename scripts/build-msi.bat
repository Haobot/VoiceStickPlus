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

:: Built-in credentials: extract volcengine + tencent + DeepSeek LLM credentials from
:: the local config.toml via a helper ps1 (avoids bat/PowerShell quote-escaping pitfalls).
:: ps1 emits "VOICESTICK_BUILTIN_<NAME>=<value>" lines; loop sets each as an env var.
:: Baked into VoiceStick.exe at compile time; Active*() accessors fall back to them on
:: first launch, skipping the ASR onboarding step for new users.
:: See Doc/Plan/windows-builtin-api-key.md.
:: P0-4 note: 本项目当前为内部测试模式，默认嵌入内置凭据（开箱即用免输 API Key）；
:: 显式 VOICESTICK_EMBED_BUILTIN_KEYS=0 才构建无凭据公开包。配套发布门禁
:: scan_release_artifacts.py 需加 --allow-builtin（内测包命中仅告警不阻断）。
if not defined VOICESTICK_EMBED_BUILTIN_KEYS set "VOICESTICK_EMBED_BUILTIN_KEYS=1"
if /I "%VOICESTICK_EMBED_BUILTIN_KEYS%"=="1" (
    for /f "usebackq tokens=1,* delims==" %%a in (`powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0extract_builtin_key.ps1"`) do set "%%a=%%b"
    if not defined VOICESTICK_BUILTIN_API_KEY (
        echo WARNING: VOICESTICK_EMBED_BUILTIN_KEYS=1 but volcengine_api_key not found; exe has no built-in ASR key.
    ) else (
        echo Injecting built-in credentials into VoiceStick.exe [OPT-IN]
    )
) else (
    echo NOTICE: VOICESTICK_EMBED_BUILTIN_KEYS=0 - building WITHOUT built-in credentials.
)
cmake -S "%WINDOWS_DIR%" -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
    -DVOICESTICK_BUILTIN_API_KEY="%VOICESTICK_BUILTIN_API_KEY%" ^
    -DVOICESTICK_BUILTIN_TENCENT_SECRET_ID="%VOICESTICK_BUILTIN_TENCENT_SECRET_ID%" ^
    -DVOICESTICK_BUILTIN_TENCENT_SECRET_KEY="%VOICESTICK_BUILTIN_TENCENT_SECRET_KEY%" ^
    -DVOICESTICK_BUILTIN_TENCENT_APPID="%VOICESTICK_BUILTIN_TENCENT_APPID%" ^
    -DVOICESTICK_BUILTIN_LLM_API_KEY="%VOICESTICK_BUILTIN_LLM_API_KEY%" ^
    -DVOICESTICK_BUILTIN_LLM_BASE_URL="%VOICESTICK_BUILTIN_LLM_BASE_URL%" ^
    -DVOICESTICK_BUILTIN_LLM_MODEL="%VOICESTICK_BUILTIN_LLM_MODEL%"
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
:: Local ASR runtime deps (copied from third_party/sherpa-onnx by CMake file(COPY)).
:: VoiceStick.exe implicitly links sherpa-onnx-c-api.dll, which loads onnxruntime.dll;
:: missing either file makes the installed app fail at launch with a loader error.
if not exist "%BUILD_DIR%\sherpa-onnx-c-api.dll" (
    echo ERROR: sherpa-onnx-c-api.dll not found in build directory.
    exit /b 1
)
if not exist "%BUILD_DIR%\onnxruntime.dll" (
    echo ERROR: onnxruntime.dll not found in build directory.
    exit /b 1
)
if not exist "%BUILD_DIR%\VoiceStickFlash.exe" (
    echo ERROR: VoiceStickFlash.exe not found in build directory.
    exit /b 1
)
if not exist "%BUILD_DIR%\VoiceStickHidTap.dll" (
    echo ERROR: VoiceStickHidTap.dll not found in build directory.
    exit /b 1
)
if not exist "%BUILD_DIR%\VoiceStickTapInject.exe" (
    echo ERROR: VoiceStickTapInject.exe not found in build directory.
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
    set SIGN_ARGS=/v /fd sha256 /sha1 %SIGNING_SHA1% /tr http://timestamp.sectigo.com /td sha256
) else (
    set SIGN_ARGS=/v /fd sha256 /a /uw /tr http://timestamp.sectigo.com /td sha256
)
:: Third-party ASR runtime DLLs shipped inside the MSI (sherpa-onnx + onnxruntime);
:: signing them like WinSparkle.dll keeps a consistent publisher across all packaged
:: binaries. All sign calls go through :SignAndVerify below, which retries once per
:: transient Defender real-time-scan hook (CRYPT_E_BAD_ENCODE 0x80093102) that
:: intermittently fails random files during SignerSign.
call :SignAndVerify "%BUILD_DIR%\VoiceStick.exe" || exit /b 1
call :SignAndVerify "%BUILD_DIR%\WinSparkle.dll" || exit /b 1
call :SignAndVerify "%BUILD_DIR%\sherpa-onnx-c-api.dll" || exit /b 1
call :SignAndVerify "%BUILD_DIR%\onnxruntime.dll" || exit /b 1
call :SignAndVerify "%BUILD_DIR%\VoiceStickFlash.exe" || exit /b 1
:: usage tap 探针 DLL 与提权注入器（增强按键识别链路）：注入系统 HID 宿主的
:: 组件必须签名（AV/SmartScreen 关注度低；注入器部署前按签名后的文件做
:: SHA-256 一致性校验）。
call :SignAndVerify "%BUILD_DIR%\VoiceStickHidTap.dll" || exit /b 1
call :SignAndVerify "%BUILD_DIR%\VoiceStickTapInject.exe" || exit /b 1

:: Step 3: Build MSI with WiX
echo.
echo [3/4] Building MSI with WiX...
:: 准备 VoiceStickFlash 自包含 esptool 运行时（python-embed + esptool，幂等），
:: 打进 MSI 的 FlashTool\ 目录。详见 Doc/Plan/windows-com-flash-tool.md。
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0prepare_flash_payload.ps1" -OutputDir "%BUILD_DIR%\flash_payload"
if errorlevel 1 (
    echo ERROR: prepare_flash_payload failed.
    exit /b 1
)
:: P0-4: the real-key config.template.toml is now explicit opt-in. Unless
:: VOICESTICK_MSI_CONFIG_SOURCE is set (or VOICESTICK_MSI_EMBED_REAL_KEYS=1), the
:: placeholder template copied by CMake POST_BUILD stays in place - public MSI has no keys.
if not defined VOICESTICK_MSI_CONFIG_SOURCE (
    if /I "%VOICESTICK_MSI_EMBED_REAL_KEYS%"=="1" set "VOICESTICK_MSI_CONFIG_SOURCE=%APPDATA%\VoiceStick\config.toml"
)
if defined VOICESTICK_MSI_CONFIG_SOURCE (
    if not exist "%VOICESTICK_MSI_CONFIG_SOURCE%" (
        echo WARNING: MSI config source not found: %VOICESTICK_MSI_CONFIG_SOURCE%
        echo          Using placeholder template from resources\config.template.toml
    ) else (
        echo Generating MSI config with real keys from: %VOICESTICK_MSI_CONFIG_SOURCE% [OPT-IN]
        powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0generate_msi_config.ps1" ^
            -SourceConfig "%VOICESTICK_MSI_CONFIG_SOURCE%" ^
            -TemplateConfig "%PROJECT_DIR%\desktop\windows\resources\config.template.toml" ^
            -OutputConfig "%BUILD_DIR%\config.template.toml"
        if errorlevel 1 (
            echo ERROR: Failed to generate MSI config from test keys.
            exit /b 1
        )
    )
) else (
    echo NOTICE: MSI config uses placeholder template - no real keys embedded [OPT-IN off]
)
:: Optional: inject a real config (with secrets) via VOICESTICK_CONFIG_TEMPLATE to override the placeholder.
:: Used to distribute a pre-configured MSI to testers; secrets are injected only at local build time, never committed.
:: On first launch the exe-adjacent config.template.toml is copied to %APPDATA%\VoiceStick\config.toml if absent.
:: See Doc/Plan/windows-msi-config-template-seed.md for details.
if defined VOICESTICK_CONFIG_TEMPLATE (
    if exist "%VOICESTICK_CONFIG_TEMPLATE%" (
        echo Overriding MSI config template from: %VOICESTICK_CONFIG_TEMPLATE%
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
:: WiX 4.0 一次构建只产一个 culture（-culture 为单值过滤，多语言 MSI 支持尚未落地），
:: 因此分别产出 zh-CN 与 en-US 两个 MSI，各自语言码（2052 / 1033）正确。
for %%C in (zh-CN en-US) do (
    echo Building %%C MSI...
    "%WIX_PATH%" build "%WINDOWS_DIR%\installer\VoiceStick.wxs" ^
        "%BUILD_DIR%\flash_payload.wxs" ^
        "%WINDOWS_DIR%\installer\%%C.wxl" ^
        -arch x64 ^
        -culture %%C ^
        -ext WixToolset.UI.wixext ^
        -ext WixToolset.Util.wixext ^
        -d ProductVersion=%VERSION% ^
        -d BuildDir=%BUILD_DIR% ^
        -d FlashPayloadDir=%BUILD_DIR%\flash_payload ^
        -d ProjectDir=%PROJECT_DIR% ^
        -o "%BUILD_DIR%\VoiceStick_%VERSION%_%%C.msi"
    if errorlevel 1 (
        echo ERROR: WiX build failed for %%C.
        exit /b 1
    )
)

:: Step 4: Sign MSI installers
echo.
echo [4/4] Signing MSI...
for %%C in (zh-CN en-US) do (
    echo Signing VoiceStick_%VERSION%_%%C.msi...
    call :SignAndVerify "%BUILD_DIR%\VoiceStick_%VERSION%_%%C.msi" || exit /b 1
)

:: Step 5: Verify MSI contents (post-build gate). The packaging list is hand-maintained;
:: v2.4.1 shipped without sherpa-onnx-c-api.dll/onnxruntime.dll and nothing downstream
:: caught it. This fails the build if any required top-level file is missing from the MSI.
echo.
echo Step 4b: Verifying MSI contents...
for %%C in (zh-CN en-US) do (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0verify_msi_contents.ps1" -Msi "%BUILD_DIR%\VoiceStick_%VERSION%_%%C.msi"
    if errorlevel 1 (
        echo ERROR: %%C MSI content verification failed.
        exit /b 1
    )
)

echo.
echo Success:
echo   %BUILD_DIR%\VoiceStick_%VERSION%_zh-CN.msi
echo   %BUILD_DIR%\VoiceStick_%VERSION%_en-US.msi
exit /b 0

:: 签名单个文件并验证。Defender 实时扫描会在 SignerSign 写证书表时钩住文件，
:: 间歇性返回 CRYPT_E_BAD_ENCODE (0x80093102) 且随机命中不同文件——对失败重试
:: （最多 3 次尝试，间隔 2s），重试仍败才算签名失败。
:: 用法: call :SignAndVerify "<文件路径>" || exit /b 1
:SignAndVerify
set "SIGN_TARGET=%~1"
set /a SIGN_ATTEMPT=0
:SignAndVerifyRetry
"%SIGNTOOL%" sign %SIGN_ARGS% "%SIGN_TARGET%"
if not errorlevel 1 goto SignAndVerifyCheck
set /a SIGN_ATTEMPT+=1
if !SIGN_ATTEMPT! GEQ 3 (
    echo ERROR: Signing "!SIGN_TARGET!" failed after !SIGN_ATTEMPT! attempts.
    exit /b 1
)
echo Signing "!SIGN_TARGET!" failed ^(attempt !SIGN_ATTEMPT!^), retrying in 2s...
ping -n 3 127.0.0.1 >nul
goto SignAndVerifyRetry
:SignAndVerifyCheck
powershell -NoProfile -Command "$sig = Get-AuthenticodeSignature -FilePath '!SIGN_TARGET!'; if ($sig.SignerCertificate) { exit 0 }; exit 1"
if errorlevel 1 (
    echo ERROR: "!SIGN_TARGET!" is not signed.
    exit /b 1
)
goto :eof
