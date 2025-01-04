#!/bin/bash

# disable WINE debug messages
export WINEDEBUG=-all

# mod folder location and naming
modout="mods-out"
moddir="$modout"/"$(date +"%y%m%d-%H%M")"
# resources directories
resdir="res"
rextdir="res-unpacked"
luadir="lua"

# stop execution if rsync/wine/progress is missing
function checkCommands {
    for cmd in wine rsync; do
        if ! command -v "$cmd" &>/dev/null; then
            echo "Error : command '$cmd' is required"
            exit 1
        fi
    done
}

# create the mod directory structure
function directoryCreation {
    echo "CREATED MOD DIRECTORY: $moddir"
    mkdir -vp "$moddir"/{config,res,maps}
}

function ini2Reg {
    local inidir="ini"
    echo "======================================== PROCESSING INI FILES ========================================"
    echo "Converting INI files to REG"

    for iniin in "$inidir"/*.ini; do
        wine bin/ini2reg.exe "$iniin" 
    done

    echo "Copying REG files into their respective folder…"
    mv -v "$inidir"/{config,autorunpro}.reg "$moddir" 2>/dev/null
    mv -v "$inidir"/{ai,music,streamsn}.reg "$moddir"/config 2>/dev/null
    mv -v "$inidir"/smessbase.reg "$moddir"/res 2>/dev/null
}

function copyMaps {
    local mapsdir="maps"
    echo "======================================== COPYING MAPS ========================================"
    rsync -rv "$mapsdir"/ "$moddir/maps"
}

function copyHdPack {
    local hdpackdir="hdlands"
    echo "======================================== COPYING HD PACK ========================================"
    rsync -rv "$hdpackdir"/ "$moddir/hdlands"
}

function dds2Mmp {
    local resddsdir="res-dds"
    local texturesddsdir="$resddsdir/textures_res_dds"
    local redressddsdir="$resddsdir/redress_res_dds"
    local texturesmmpdir="$rextdir/textures_res"
    local redressmmpdir="$rextdir/redress_res"
    echo "======================================== PROCESSING DDS FILES ========================================"

    if [[ "$redressAnswer" == "y" ]]; then
        cd $redressddsdir || exit
        for redressdds in *.dds; do
            local redressmmpout="${redressdds%dds}mmp"
            wine ../../bin/MMPS.exe "$redressdds" && mv -fv "$redressmmpout" ../../"$redressmmpdir"
        done
        cd ../..
        echo "Processed redress"
    fi

    if [[ "$texturesAnswer" == "y" ]]; then
        cd $texturesddsdir || exit
        for texturesdds in *.dds; do
            local texturesmmpout="${texturesdds%dds}mmp"
            wine ../../bin/MMPS.exe "$texturesdds" && mv -fv "$texturesmmpout" ../../"$texturesmmpdir"
        done
        cd ../..
        echo "Processed textures"
    fi
}

function eiDbEditor {
    local xlsxdir="xlsx"
    echo "======================================== PROCESSING DATABASELMP ========================================"
    cd "$xlsxdir" || exit
    echo "Converting XLSX databaselmp to RES..."
    wine start /wait ../bin/eidbeditor-144/DBEditor.exe databaselmp.xlsx && mv -fv databaselmp.res ../"$resdir"/
    cd ..
}

function writeVersion {
    # version hash shortened and version file are declared globally to avoid "Declare and assign separately to avoid masking return values."
    commithashshort="$(git rev-parse --short HEAD)"
    versionfile="version/mod-version.txt"
    version=$(cat "$versionfile")
    local resversionname="res-unpacked/texts_res/string version_name"
    local versiontemplate="version/version-name-format.txt"
    echo "======================================== WRITING VERSION ========================================"

    if [[ "$wvAnswer" == "y" ]]; then
        IFS='.' read -r major minor patch <<<"$version"
        if ((patch < 9)); then
            ((patch++))
        else
            patch=0
            if ((minor < 9)); then
                ((minor++))
            else
                minor=0
                ((major++))
            fi
        fi
        version="$major.$minor.$patch"
        echo "$version" >"$versionfile"
    fi

    cp -vL "$versiontemplate" "$resversionname"
    sed -i "s/ver\./ver. $version/" "$resversionname"
    sed -i "s/commit\./commit. $commithashshort/" "$resversionname"

    echo "File $resversionname updated with version $version and commit hash $commithashshort"
}

function eiPacker {
    echo "======================================== PACKING RES FILES ========================================"

    for rextin in "$rextdir"/*_res; do
        local resout="${rextin%_res}.res"
        wine bin/eipacker.exe /pack "$rextin" && rsync -rv --remove-source-files "$resout" "$resdir"
    done

    echo "Deleting empty directories"
    find ./"$rextin" -depth -type d -empty -delete
    echo "Moving RES files into $moddir/res"
    mv -fv "$resdir"/*.res "$moddir"/res
}

function addLua {
    echo "======================================== ADDING LUA SCRIPTS ========================================"
    cp -vL "$luadir"/main.lua "$moddir"
    cp -vrL "$luadir"/lua "$moddir"/lua
}

function replaceOldMod {
    if [[ "$mvAnswer" != "n" ]]; then
        rsync -r "$moddir"/ ../../Universal-Mod
        echo "FILES MOVED TO MOD RELEASE DIRECTORY"
    fi
}

echo "Welcome to the Evil Islands Auto-Compiler script for GNU/Linux !"
echo "Please press y or n to configure the options, or press Enter to keep the default settings :"
read -rp "Convert REDRESS from DDS to MMP ? (y/N) " redressAnswer
read -rp "Convert TEXTURES from DDS to MMP ? (y/N) " texturesAnswer
read -rp "Increment version ? (y/N) " wvAnswer
read -rp "Replace the older mod files ? (Y/n)" mvAnswer
echo ""

# CALLING FUNCTIONS IN THE RIGHT ORDER
checkCommands
directoryCreation
ini2Reg
copyMaps
copyHdPack
eiDbEditor
dds2Mmp
writeVersion
eiPacker
addLua
replaceOldMod

echo ""
echo "DONE PROCESSING $moddir | MOD VERSION $version | COMMIT HASH $commithashshort"
echo ""
