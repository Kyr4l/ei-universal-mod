#!/bin/bash

# ressource folders
ini=ini
reg=reg

totalreg=""

cd $ini || exit

for in in *.ini; do
    out="${in%ini}reg"
    wine ../bin/ini2reg.exe "$in"
    mv -fv "$out" ../$reg/
    echo "Processed $in -> $out"
    totaldds="$totaldds $in"
done

echo "DONE CONVERTING FILES TO REG"
echo "Processed the folllwing file(s) : $totalreg"
