@echo off
rem Writes every texture of the game as a PNG into the textures folder beside this file.
rem Needs Python 3.9 or later. The game must have been started once, so that ChiliNative.ini names the disc.
setlocal
cd /d "%~dp0"
set "PY=python"
where py >nul 2>nul && set "PY=py -3"
%PY% --version >nul 2>nul
if errorlevel 1 goto no_python
%PY% "tools\dump_textures.py" %*
echo.
pause
exit /b

:no_python
echo Python 3.9 or later is needed for this: https://www.python.org/downloads/
pause
exit /b 1
