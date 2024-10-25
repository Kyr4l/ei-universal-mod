@cho off

echo -------------
echo Build Russian
echo -------------
call Utils\MakeMQ.bat Russian

echo -------------
echo Build English
echo -------------
call Utils\MakeMQ.bat English

echo ------------
echo Build German
echo ------------
call Utils\MakeMQ.bat German

echo ------------------------------
echo Build successfully compleated.
echo ------------------------------
pause