@echo off
setlocal enabledelayedexpansion

set "resdir=.\res"
set "reslangdir=%resdir%\lang"

set "languages="

for %%f in (%reslangdir%\texts-*res) do (
    set filename=%%~nf
    set langcode=!filename:~6,3!

    if exist %reslangdir%\textslmp-!langcode!.res (
        set languages=!languages! !langcode!
    ) else (
        echo !langcode! is incomplete!
    )
)

echo.
echo =====================================
echo UNIVERSAL MOD LANGUAGE SWITCHING TOOL
echo =====================================
echo.
echo Detected Languages:!languages!
echo.

if "!languages!"=="" (
    echo No languages found. Exiting.
    exit /b 1
)

echo Please type the target language code and press ENTER:
echo.
for %%l in (!languages!) do (
    echo - %%l
)
echo.

:chooseLang
set /p langnswr=Enter the language code (or 'q' to quit): 
if /i "!langnswr!"=="q" (
    echo Exiting.
    exit /b 0
)
echo.

if not "!langnswr:~3,1!"=="" (
    echo Invalid language code length. Please try again.
    goto chooseLang
)
set "found=0"
for %%l in (!languages!) do (
    if /i "%%l"=="!langnswr!" set "found=1"
)
if "!found!"=="0" (
    echo Invalid language code. Please try again.
    goto chooseLang
)

if exist "%resdir%\texts.res" (
    del "%resdir%\texts.res"
    echo Deleted old texts.res
)
if exist "%resdir%\textslmp.res" (
    del "%resdir%\textslmp.res"
    echo Deleted old textslmp.res
)
echo.

if exist "%reslangdir%\texts-!langnswr!.res" (
    copy "%reslangdir%\texts-!langnswr!.res" "%resdir%\texts.res"
    echo Copied texts-!langnswr!.res to texts.res
)
if exist "%reslangdir%\textslmp-!langnswr!.res" (
    copy "%reslangdir%\textslmp-!langnswr!.res" "%resdir%\textslmp.res"
    echo Copied textslmp-!langnswr!.res to textslmp.res
)
echo.

echo Language switched to !langnswr!. Files have been copied and renamed successfully.

endlocal
