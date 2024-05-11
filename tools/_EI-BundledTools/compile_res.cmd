@echo on
setlocal enabledelayedexpansion

rem Compiled resources folder
set res=res
rem Decompiled resources folder
set rext=res_extracted

cd %rext%

for %%i in (*_res) do (
    set out=!i:_res=.res!
    ..\bin\eipacker.exe /pack %%i
    echo Done %%i -> !out!
    move !out! ..\%res%\
)
endlocal

echo DONE PACKING ASSETS
