@echo off
rem Configures (first run) and builds the GE renderer bring-up project into
rem out\chili-gpu with Ninja. Usage: build.cmd [target]
setlocal
set "HERE=%~dp0"
set "ROOT=%HERE%..\..\.."
set "OUT=%ROOT%\out\chili-gpu"
set "VSROOT=C:\Program Files\Microsoft Visual Studio\18\Community"
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
if not exist "%OUT%\build.ninja" (
    cmake -S "%HERE%." -B "%OUT%" -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
)
if "%~1"=="" (
    cmake --build "%OUT%" || exit /b 1
) else (
    cmake --build "%OUT%" --target %* || exit /b 1
)
endlocal
