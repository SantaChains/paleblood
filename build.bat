@echo off
rem Windows build entry (scripts/build_windows.py): compiles the port through MSYS2. Separate
rem from run.bat, which starts the game directly without building. Output: out\build-launcher.log.
setlocal
pushd "%~dp0"
rem Chinese/Japanese Windows default python pipes to the legacy ANSI codepage (GBK), which
rem cannot encode characters like the trademark sign in source/game file names. The console
rem code page follows (chcp), so tool output with non-ASCII text displays correctly as well.
chcp 65001 >nul
set "PYTHONUTF8=1"
rem Parent processes (e.g. an IDE started before BB_MSYS2 was set) carry a stale environment:
rem read the user variable from the registry instead of trusting the inherited one.
if not defined BB_MSYS2 for /f "tokens=2,*" %%a in ('reg query HKCU\Environment /v BB_MSYS2 2^>nul ^| findstr BB_MSYS2') do set "BB_MSYS2=%%b"
if not defined BB_MSYS2 set "BB_MSYS2=C:\msys64"
set "BB_PYTHON=%BB_MSYS2%\clang64\bin\python.exe"
if not exist "%BB_PYTHON%" (
    echo MSYS2 Python not found at %BB_PYTHON%. Install MSYS2 and the packages listed in README.md, or set BB_MSYS2.
    pause
    exit /b 1
)
"%BB_PYTHON%" "scripts\build_windows.py" %*
set "RC=%ERRORLEVEL%"
echo.
if "%RC%"=="0" (
    echo build.bat: done. Full log: out\build-launcher.log.
) else (
    echo build.bat: the build failed. The log tail is above, full log: out\build-launcher.log.
)
rem Double-clicked windows vanish before the result can be read; pause only then.
echo "%cmdcmdline%" | findstr /i /c:"%~nx0" >nul && pause
popd
exit /b %RC%
