#!/bin/bash

# disable WINE debug messages
export WINEDEBUG=-all

# ressource folders
dds="dds"
mmp="mmp"

totalmmp=""

cd "$dds" || exit

for in in *.dds; do
    out="${in%dds}mmp"
    wine ../bin/MMPS.exe "$in"
    mv -fv "$out" ../$mmp/
    echo "Processed $in -> $out"
    totalmmp="$totalmmp $in"
done

echo "DONE CONVERTING FILES TO MMP"
echo "Processed the folllwing files : $totalmmp"
