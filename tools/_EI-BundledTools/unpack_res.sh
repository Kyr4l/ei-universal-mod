#!/bin/bash

# packed res folder
res="res"
# unpacked res folder
rext="res_unpacked"

totalunpacked=""

cd $res || exit

for in in *.res; do
 out="${in%.res}_res"
 wine ../bin/eipacker.exe "$in"
 echo "Done unpacking $in -> $out"
 rsync -r --remove-source-files ./"$out" ../"$rext"/
 echo "RSync completed on $out"
 find . -depth -type d -empty -delete
 echo "Empty directories deleted"
 totalunpacked="$totalunpacked $in"
done

echo "DONE UNPACKING ASSETS"
echo "Processed the following file(s) : $totalunpacked"
