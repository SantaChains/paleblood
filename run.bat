@echo off
rem Windows counterpart of run.sh (scripts/run_windows.py): starts the game directly. Building
rem is separate: build.bat (or run.bat --build). Usage: run.bat [--game-dir DIR] [bb-probe options...]
rem MSYS2 is expected in C:\msys64 (set BB_MSYS2 otherwise); see README "Windows".
setlocal
pushd "%~dp0"
rem Chinese/Japanese Windows default python pipes to the legacy ANSI codepage (GBK), which
rem cannot encode characters like the trademark sign in game/mod file names. The console code
rem page follows (chcp), so game output with non-ASCII text displays correctly as well.
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
rem First run: ask for the game dump once and remember it (run_windows.py reads out\game_dir.txt).
if exist "out\game_dir.txt" goto have-game
if defined BB_GAME_DIR goto have-game
echo %* | findstr /C:"--game-dir" >nul && goto have-game
set /p "GAME_DIR=First run: enter the path to your Bloodborne 1.09 dump (the folder containing eboot.bin): "
if not defined GAME_DIR exit /b 1
if not exist "%GAME_DIR%\eboot.bin" (
    echo No eboot.bin in "%GAME_DIR%" - that folder is not a game dump.
    pause
    exit /b 1
)
mkdir out 2>nul
>"out\game_dir.txt" echo %GAME_DIR%
:have-game

"%BB_PYTHON%" "scripts\run_windows.py" %*
set "RC=%ERRORLEVEL%"
if not "%RC%"=="0" (
    echo.
    echo run.bat: the launcher exited with code %RC%. This window stays open so the error above can be read.
    pause
)
popd
exit /b %RC%
