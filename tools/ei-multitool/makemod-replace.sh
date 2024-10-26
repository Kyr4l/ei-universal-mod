#!/bin/bash

# calling makemod.sh using source allows catching the variables and performs early checks
source ./makemod.sh

# copy modfiles to release
# var $modfolder returned from makemod.sh thru source
rsync -r "$moddir"/ ../../Universal-Mod

# echo rsync
echo "FILES MOVED TO MOD RELEASE DIRECTORY"
