@echo off
rem Starts Chili Con Carnage (native PC build) from the build tree. See BUILDING.md at the repository root.
rem The game saves into game\ms0_play, a copy of game\ms0 made on the first start; game\ms0 is never written.
setlocal
set "HERE=%~dp0"
set "BIN=%HERE%..\..\out\chili-rel\bin"
set "EXE=%BIN%\chili_recomp.exe"
set "DISC=%HERE%game\disc0"
set "BOOT=%DISC%\PSP_GAME\SYSDIR\BOOT.BIN"
set "SAVE=%HERE%game\ms0_play"
set "LOG=%HERE%..\..\out\chili-rel\last_run.log"

if not exist "%EXE%" goto no_exe
if not exist "%BOOT%" goto no_disc
if exist "%SAVE%\" goto start
if not exist "%HERE%game\ms0\" goto start
echo First start: copying the save folder game\ms0 to game\ms0_play
robocopy "%HERE%game\ms0" "%SAVE%" /E /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 goto no_copy

:start
echo Chili Con Carnage is running. Close the game window to quit.
echo Log: %LOG%
cd /d "%BIN%"
"%EXE%" "%BOOT%" "%DISC%" --ms0 "%SAVE%" > "%LOG%" 2>&1
if errorlevel 1 goto failed
exit /b 0

:failed
set "CODE=%ERRORLEVEL%"
echo.
echo The game ended with exit code %CODE%. Last lines of the log:
powershell -NoProfile -Command "Get-Content -LiteralPath $env:LOG -Tail 15"
pause
exit /b %CODE%

:no_exe
echo The game is not built: %EXE% is missing.
echo Build it with: powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%scripts\build.ps1" -Config rel
pause
exit /b 2

:no_disc
echo The disc files are missing: %BOOT%
pause
exit /b 2

:no_copy
echo Copying the save folder failed; nothing was started.
pause
exit /b 2
