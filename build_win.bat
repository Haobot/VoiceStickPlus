@echo off
setlocal

cd /d "%~dp0"
set "ROOT_DIR=%cd%"
set "SOURCE_DIR=%ROOT_DIR%\desktop\windows"
set "BUILD_DIR=%SOURCE_DIR%\build-x64"
set "LOG_FILE=%ROOT_DIR%\build_output.log"
set "SETUP_LOG=%ROOT_DIR%\build_setup.log"
set "CONFIGURE_LOG=%ROOT_DIR%\build_configure.log"
set "BUILD_LOG=%ROOT_DIR%\build_compile.log"
set "VS_INSTALL_PATH="

> "%LOG_FILE%" echo Starting build at %date% %time%
> "%SETUP_LOG%" echo === MSVC environment setup log ===
> "%CONFIGURE_LOG%" echo === CMake configure log ===
> "%BUILD_LOG%" echo === Build log ===

echo.
echo [1/4] Cleaning up leftover processes...
>> "%LOG_FILE%" echo === Killing leftover processes ===

taskkill /f /im VoiceStick.exe /t >> "%LOG_FILE%" 2>&1
if not errorlevel 1 echo   - Terminated running VoiceStick.exe
taskkill /f /im ninja.exe /t >> "%LOG_FILE%" 2>&1
taskkill /f /im cmake.exe /t >> "%LOG_FILE%" 2>&1
taskkill /f /im cl.exe /t >> "%LOG_FILE%" 2>&1
taskkill /f /im link.exe /t >> "%LOG_FILE%" 2>&1
timeout /t 1 /nobreak > nul 2>&1

echo.
echo [2/4] Cleaning previous build output...
>> "%LOG_FILE%" echo === Cleaning build directory ===

if exist "%BUILD_DIR%\VoiceStick.exe" del /f /q "%BUILD_DIR%\VoiceStick.exe" >> "%LOG_FILE%" 2>&1
if exist "%BUILD_DIR%" (
    rd /s /q "%BUILD_DIR%" >> "%LOG_FILE%" 2>&1
    if errorlevel 1 (
        echo   Warning: failed to fully clean build directory
        echo   Some files may still be locked, trying a fresh configure anyway
    ) else (
        echo   - Build directory cleaned
    )
)

del /f /q "%ROOT_DIR%\build_output.log.lock" > nul 2>&1

echo.
echo [3/4] Setting up MSVC build environment...
>> "%LOG_FILE%" echo === Setting up MSVC environment ===

for %%V in (Community BuildTools Professional Enterprise) do (
    if not defined VS_INSTALL_PATH if exist "C:\Program Files\Microsoft Visual Studio\2022\%%V\VC\Auxiliary\Build\vcvars64.bat" (
        set "VS_INSTALL_PATH=C:\Program Files\Microsoft Visual Studio\2022\%%V"
    )
    if not defined VS_INSTALL_PATH if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\%%V\VC\Auxiliary\Build\vcvars64.bat" (
        set "VS_INSTALL_PATH=C:\Program Files (x86)\Microsoft Visual Studio\2022\%%V"
    )
)

if not defined VS_INSTALL_PATH (
    echo ERROR: Visual Studio 2022 not found
    >> "%LOG_FILE%" echo ERROR: Visual Studio 2022 not found
    exit /b 1
)

echo   - Using Visual Studio 2022 from: %VS_INSTALL_PATH%
call "%VS_INSTALL_PATH%\VC\Auxiliary\Build\vcvars64.bat" >> "%SETUP_LOG%" 2>&1
if errorlevel 1 (
    echo ERROR: Failed to setup MSVC environment
    echo See build_setup.log for details
    exit /b 1
)

echo.
echo [4/4] Running CMake build...
>> "%CONFIGURE_LOG%" echo.
>> "%CONFIGURE_LOG%" echo === Running CMake configuration ===
cmake -S "%SOURCE_DIR%" -B "%BUILD_DIR%" -G Ninja >> "%CONFIGURE_LOG%" 2>&1
if errorlevel 1 (
    echo ERROR: CMake configuration failed
    echo.
    echo See build_configure.log for details
    exit /b 1
)

>> "%BUILD_LOG%" echo.
>> "%BUILD_LOG%" echo === Running build ===
cmake --build "%BUILD_DIR%" >> "%BUILD_LOG%" 2>&1
if errorlevel 1 (
    echo ERROR: Build failed
    echo.
    echo See build_compile.log for details
    exit /b 1
)

echo.
echo Build SUCCEEDED! Output: %BUILD_DIR%\VoiceStick.exe
>> "%LOG_FILE%" echo Build SUCCEEDED at %date% %time%
>> "%LOG_FILE%" echo Setup log: %SETUP_LOG%
>> "%LOG_FILE%" echo Configure log: %CONFIGURE_LOG%
>> "%LOG_FILE%" echo Build log: %BUILD_LOG%

endlocal
exit /b 0
