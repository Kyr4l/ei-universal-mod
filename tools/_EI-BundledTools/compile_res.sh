#!/bin/bash

# compiled res folder
res="res"
# decompiled res folder
rext="res_extracted"

cd $rext

for in in *_res; do
 out="${in%_res}.res"
 wine ../bin/eipacker.exe /pack $in
 echo "Done $in -> $out"
 mv ./$out ../"$res"/"$out"
done
