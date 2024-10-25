#!/bin/bash

# disable WINE debug messages
export WINEDEBUG=-all

# ressource folders
dds="dds"
mmp="mmp"

totaldds=""

cd "$mmp" || exit

function mmp2DDS {
for in in *.mmp; do
    out="${in%mmp}dds"
    wine ../bin/MMPS.exe "$in"
    mv -fv "$out" ../"$dds"/
    echo "Processed $in -> $out"
    totaldds="$totaldds $in"
done
}
mmp2DDS

echo "DONE CONVERTING FILES TO DDS"
echo "Processed the following files : $totaldds"
