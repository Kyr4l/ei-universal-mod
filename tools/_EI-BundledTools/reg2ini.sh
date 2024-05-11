#!/bin/bash

# ressource folders
ini=ini
reg=reg

totalini=""

cd $reg || exit

for in in *.reg; do
    out="${in%reg}ini"
    wine ../bin/reg2ini.exe "$in"
    mv -fv "$out" ../$ini/
    echo "Processed $in -> $out"
    totaldds="$totaldds $in"
done

echo "DONE CONVERTING FILES TO INI"
echo "Processed the folllwing file(s) : $totalini"
