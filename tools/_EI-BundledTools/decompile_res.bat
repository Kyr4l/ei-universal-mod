@echo off

rem Compiled resources folder
set res=res
rem Decompiled resources folder
set rext=res_extracted

cd %res%

for %%i in (*.res) do (
    set out=%%~ni_res
    ..\bin\eipacker.exe %%i
    echo Done %%i -> !out!
    move !out! ..\%rext%\!out!
)
