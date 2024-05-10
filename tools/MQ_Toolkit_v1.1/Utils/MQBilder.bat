@echo off
@rem ****************** Check compleate quest files ********************
if "%1" == "" goto QuestZone
if "%2" == "" goto QuestNum
if "Texts\%3" == "" goto QuestLang
if not exist Texts\%3\"briefing "z%1q%2"_1" goto QuestBrief1
if not exist Texts\%3\"briefing "z%1q%2"_2" goto QuestBrief2
if not exist Texts\%3\"briefing "z%1q%2"_3" goto QuestBrief3
if not exist Texts\%3\"quest "z%1q%2 goto QuestSubobj
if not exist Quests\z%1q%2\map.txt goto QuestMap
if not exist Quests\z%1q%2\quest.Ini goto QuestIni
if not exist Utils\Ini2Reg.exe goto Ini2Reg
if not exist Utils\ResBuild.exe goto ResBuild

@rem ****************** Compile quest files ****************************
if not exist mq mkdir mq
if not exist mq\%3 mkdir mq\%3
if exist mq\%3\z%1q%2.mq del mq\%3\z%1q%2.mq

cd Texts\%3
..\..\Utils\ResBuild a ..\..\mq\%3\z%1q%2.mq "briefing "z%1q%2"_1" "briefing "z%1q%2"_2" "briefing "z%1q%2"_3" "quest "z%1q%2
cd ..\..

cd Quests\z%1q%2
..\..\Utils\Ini2Reg quest.Ini
cd ..

..\Utils\ResBuild a ..\mq\%3\z%1q%2.mq z%1q%2\map.txt z%1q%2\quest.reg
if exist z%1q%2\quest.reg del z%1q%2\quest.reg
cd ..

goto _exit

@rem ****************** Error messages ********************************
:QuestZone
echo Error0 - Zone number not set
goto _err_exit

:QuestNum
echo Error0 - Quest number for zone "%1" not set
goto _err_exit

:QuestLang
echo Error0 - Quest language not set
goto _err_exit

:QuestName
echo Error1 - Quest name not found
goto _err_exit

:QuestFolder
echo Error2 - Quest folder: "Quests\z%1q%2" not found
goto _err_exit

:QuestSubFolder
echo Error3 - Quest subfolder: "z%1q%2\z%1q%2" not found
goto _err_exit

:QuestBrief1
echo Error4 - Quest Brief receive: "Texts\%3\briefing z%1q%2_1" not found
goto _err_exit

:QuestBrief2
echo Error5 - Quest Brief reject: "Texts\%3\briefing z%1q%2_2" not found
goto _err_exit

:QuestBrief3
echo Error6 - Quest Brief complete: "Texts\%3\briefing z%1q%2_3" not found
goto _err_exit

:QuestSubobj
echo Error7 - Quest Subobjectives: "Texts\%3\quest z%1q%2" not found
goto _err_exit

:QuestMap
echo Error8 - Quest map: "Quests\z%1q%2\map.txt" not found
goto _err_exit

:QuestIni
echo Error9 - Quest ini: "Quests\z%1q%2\quest.ini" not found
goto _err_exit

:Ini2Reg
echo ErrorA - File: ..\Utils\Ini2Reg.exe not found.
goto _err_exit

:ResBuild
echo ErrorB - File: ..\Utils\ResBuild.exe not found.

:_err_exit
pause

:_exit
