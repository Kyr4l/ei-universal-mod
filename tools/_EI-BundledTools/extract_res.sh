#!/bin/bash

# compiled res folder
res="res"
# decompiled res folder
rext="res_extracted"

cd $res

for in in *.res; do
 out="${in%.res}_res"
 wine ../bin/eipacker.exe $in
 echo "Done extracting $in -> $out"
 rsync -r --remove-source-files ./$out ../"$rext"/"$out"
 echo "RSync completed on $out"
 find -depth -type d -empty -delete
 echo "Empty directories deleted"
done

echo "DONE EXTRACTING ASSETS"
