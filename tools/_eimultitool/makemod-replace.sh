#!/bin/bash

# call makemod.sh
# using source allows catching the variables
source ./makemod.sh

# copy modfiles to release
# var $modfolder returned from makemod.sh thru source
rsync -rv "$modfolder"/ ../../Universal-Mod

# echo rsync
echo "FILES MOVED TO MOD RELEASE DIRECTORY"






