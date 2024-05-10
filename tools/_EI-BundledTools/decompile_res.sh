#!/bin/bash

# compiled res folder
res="res"
# decompiled res folder
rext="res_extracted"

cd $res

for in in *.res; do
 out="${in%.res}_res"
 wine ../bin/eipacker.exe $in
 echo "Done $in -> $out"
 mv ./$out ../$rext/$out
done
