@echo off
setlocal

echo ============================================
echo  Arcane Engine Project Generation
echo ============================================
echo.

set "PROJECT_ROOT=%~dp0.."

:: -------------------------------------------------------------------
:: Resolve vcpkg (required for SDL3 -- see scripts\setup-vcpkg-deps.bat)
:: -------------------------------------------------------------------
echo [1/2] Checking vcpkg...

if defined VCPKG_ROOT (
    set "VCPKG_PATH=%VCPKG_ROOT%"
) else if exist "C:\vcpkg\vcpkg.exe" (
    set "VCPKG_PATH=C:\vcpkg"
) else (
    echo ERROR: vcpkg not found.
    echo Set the VCPKG_ROOT environment variable or install vcpkg to C:\vcpkg.
    echo.
    if not defined _APH_NOPAUSE pause
    exit /b 1
)

if not exist "%VCPKG_PATH%\vcpkg.exe" (
    echo ERROR: vcpkg.exe not found at %VCPKG_PATH%
    echo Verify your VCPKG_ROOT is correct.
    echo.
    if not defined _APH_NOPAUSE pause
    exit /b 1
)
echo   vcpkg: %VCPKG_PATH%

:: Export VCPKG_ROOT into the environment so premake5.lua's os.getenv()
:: succeeds even when this script used a fallback path.
set "VCPKG_ROOT=%VCPKG_PATH%"

:: -------------------------------------------------------------------
:: Generate Visual Studio solution
:: -------------------------------------------------------------------
echo [2/2] Generating Visual Studio 2026 solution...

set "PREMAKE5=%PROJECT_ROOT%\ThirdParty\premake5\premake5.exe"
if not exist "%PREMAKE5%" (
    echo ERROR: premake5 not found at %PREMAKE5%
    echo Ensure ThirdParty\premake5\premake5.exe exists in the repo.
    echo.
    if not defined _APH_NOPAUSE pause
    exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%PROJECT_ROOT%\scripts\generate-engine-constants.ps1"
if errorlevel 1 (
    echo ERROR: generate-engine-constants.ps1 failed.
    echo.
    if not defined _APH_NOPAUSE pause
    exit /b 1
)

pushd "%PROJECT_ROOT%"
call "%PREMAKE5%" vs2026
set "PREMAKE_ERR=%ERRORLEVEL%"
popd

if %PREMAKE_ERR% NEQ 0 (
    echo ERROR: premake5 failed.
    echo.
    if not defined _APH_NOPAUSE pause
    exit /b 1
)

echo.
echo ============================================
echo  Success! Open Arcane.slnx in Visual Studio 2026.
echo ============================================
echo.
if not defined _APH_NOPAUSE pause
endlocal
