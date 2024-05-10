@echo off
setlocal enabledelayedexpansion

rem Compiled resources folder
set res=res
rem Decompiled resources folder
set rext=res_extracted

cd %rext%

for %%i in (*_res) do (
    set out=%%~ni.res
    ..\bin\eipacker.exe /pack %%i
    echo Done %%i -> !out!
    move !out! ..\%res%\!out!
)
endlocal

echo DONE PACKING ASSETS
