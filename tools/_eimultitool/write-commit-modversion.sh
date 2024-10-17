#!/bin/bash

commithashshort="$(git rev-parse --short HEAD)"
versionfile="modversion.txt"
resversionname="./res-unpacked/texts_res/string version_name"

cp "$versionfile" "$resversionname"

sed -i "s/ver\./ver. $commithashshort/" "$resversionname"

echo "File $resversionname updated with commit hash: $commithashshort"

