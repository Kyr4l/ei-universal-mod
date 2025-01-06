@echo off
@ chcp 1251
cd /d "%~dp0"
echo --------------------------------------------
echo -- HD Lands Builder
::echo -- ----------------------
echo -- Created by Atm(c)2023
echo -- -Tools by Demoth(c)2014-2016
echo -- -Tools by Nival(c)1998-2001
echo --------------------------------------------
start /b "" "%CD%\tools\ini2reg.exe" %CD%\config.ini
::start "" "%CD%\tools\ini2reg.exe" %CD%\config\streamsn.ini
::start "" "%CD%\tools\ini2reg.exe" %CD%\config\music.ini
for /D %%B in (%CD%\HDLandsSpecific\*_res) do start /b "" "%CD%\tools\eipacker.exe" %%B
for /D %%B in (%CD%\HDModelsRes\*_res) do start /b "" "%CD%\tools\eipacker.exe" %%B
for /D %%B in (%CD%\HDRes\*_res) do start /b "" "%CD%\tools\eipacker.exe" %%B
for /D %%B in (%CD%\HDTexRes\*_res) do start /b "" "%CD%\tools\eipacker.exe" %%B
::echo %%B
::pause
start "" "%CD%\..\..\EIStarter.exe"