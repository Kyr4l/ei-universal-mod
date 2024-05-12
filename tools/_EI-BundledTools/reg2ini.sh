#!/bin/bash

# disable WINE debug messages
export WINEDEBUG=-all

# ressource folders
ini="ini"
reg="reg"

totalini=""


for reg in "$reg"/*.reg ; do
    out="${reg%reg}ini"
    wine bin/reg2ini.exe "$reg"
    mv -fv "$out" "$ini"/
    echo "Processed $reg -> $out"
    totalini="$totalini $reg"
done

echo "DONE CONVERTING FILES TO INI"
echo "Processed the following file(s) : $totalini"
