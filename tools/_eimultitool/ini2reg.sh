#!/bin/bash

# disable WINE debug messages
export WINEDEBUG=-all

# ressource folders
ini="ini"
reg="reg"

totalreg=""


for ini in "$ini"/*.ini; do
    out="${ini%ini}reg"
    wine bin/ini2reg.exe "$ini"
    mv -fv "$out" $reg/
    echo "Processed $ini -> $out"
    totalreg="$totalreg $ini"
done

echo "DONE CONVERTING FILES TO REG"
echo "Processed the following files : $totalreg"
