@echo off
setlocal
set "MSYS2_ROOT=C:\msys64"
if not exist "%MSYS2_ROOT%\msys2_shell.cmd" (
    echo No se encuentra MSYS2 en C:\msys64.
    echo Si lo instalaste en otra ruta, abre MSYS2 UCRT64 y ejecuta bash COMPILAR.sh.
    pause
    exit /b 1
)
pushd "%~dp0" || exit /b 1
call "%MSYS2_ROOT%\msys2_shell.cmd" -defterm -here -no-start -ucrt64 -c "bash ./COMPILAR.sh"
set "BUILD_STATUS=%ERRORLEVEL%"
popd
if not "%BUILD_STATUS%"=="0" echo La compilacion ha terminado con un error.
pause
exit /b %BUILD_STATUS%
