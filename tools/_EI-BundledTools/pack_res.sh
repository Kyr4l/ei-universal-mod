#!/bin/bash

# packed res folder
res="res"
# unpacked res folder
rext="res_unpacked"

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
