#!/bin/bash

# disable WINE debug messages
export WINEDEBUG=-all

# packed res folder
res="res"
# unpacked res folder
rext="res-unpacked"

totalpacked=""


for in in "$rext"/*_res; do
 out="${in%_res}.res"
 wine bin/eipacker.exe /pack "$in"
 echo "Done packing $in -> $out"
 rsync -r --remove-source-files "$out" "$res"/
 echo "RSync completed on $out"
 find ./"$rext" -depth -type d -empty -delete
 echo "Empty directories deleted"
 totalpacked="$totalpacked $in"
done

echo "DONE PACKING ASSETS"
echo "Processed the following files : $totalpacked"
