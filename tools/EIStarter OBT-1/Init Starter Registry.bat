@echo off
set CURRENT_DIR=%~dp0
echo Create BackUp
reg export "HKCU\SOFTWARE\Gipat.Ru\EI_Starter" "%CURRENT_DIR%\EIS_BkUp.reg" /y
echo Add EI_Starter keys
reg add HKCU\software\Gipat.Ru /f
reg add HKCU\software\Gipat.Ru\EI_Starter /f
reg add HKCU\software\Gipat.Ru\EI_Starter\settings /f
reg add HKCU\software\Gipat.Ru\EI_Starter\EvilIslands /f
echo Clone EI registry to EI_Starter registry
reg copy "HKCU\SOFTWARE\Nival Interactive\EvilIslands" "HKCU\software\Gipat.Ru\EI_Starter\EvilIslands" /s /f
echo Task done