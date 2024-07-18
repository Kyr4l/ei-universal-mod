@echo off
@ chcp 1251
cd /d "%~dp0"
::setlocal enabledelayedexpansion
echo --------------------------------------------
echo -- HD Lands Builder
echo -- Created by Atm(c)2024
echo -- -Tools by Demoth(c)2014-2016
echo -- -Tools by Nival(c)1998-2001
echo --------------------------------------------

FOR %%i IN (%CD%\HDLandsSpecific\*.res) DO (
start /b "" %CD%\tools\eipacker.exe "%%i"
)
FOR %%i IN (%CD%\HDModelsRes\*.res) DO (
start /b "" %CD%\tools\eipacker.exe "%%i"
)
FOR %%i IN (%CD%\HDRes\*.res) DO (
start /b "" %CD%\tools\eipacker.exe "%%i"
)
FOR %%i IN (%CD%\HDTexRes\*.res) DO (
start /b "" %CD%\tools\eipacker.exe "%%i"
)
::pause
