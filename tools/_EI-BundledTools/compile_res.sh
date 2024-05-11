#!/bin/bash

# compiled res folder
res="res"
# decompiled res folder
rext="res_extracted"

cd $rext || exit

for in in *_res; do
 out="${in%_res}.res"
 wine ../bin/eipacker.exe /pack "$in"
 echo "Done packing $in -> $out"
 rsync -r --remove-source-files ./"$out" ../"$res"/
 echo "RSync completed on $out"
 find . -depth -type d -empty -delete
 echo "Empty directories deleted"
done

echo "DONE PACKING ASSETS"
