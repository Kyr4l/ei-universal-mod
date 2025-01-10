#!/bin/bash

# disable WINE debug messages
export WINEDEBUG=-all

# mod folder location and naming
moddir="mods-out"/"$(date +"%y%m%d-%H%M")"
# resources directories
resdir="res"
rextdir="res-unpacked"
reslangdir="res-texts"
luadir="lua"
inidir="ini"

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

function packLanguageTexts {
    # language specific text res
    local textseng="$reslangdir/texts_res_eng"
    local textslmpeng="$reslangdir/textslmp_res_eng"
    local textsfra="$reslangdir/texts_res_fra"
    local textslmpfra="$reslangdir/textslmp_res_fra"
    echo "======================================== PACKING TEXTS & TEXTSLMP ========================================"
    echo "Selected language: $lang"

    if [[ "$lang" == "eng" ]]; then
        local restextsout="${textseng%_eng}.eng"
        local restextslmpout="${textslmpeng%_eng}.eng"
        wine bin/eipacker.exe /pack "$textseng" && mv -v "$restextsout" "$moddir/res/texts.res"
        wine bin/eipacker.exe /pack "$textslmpeng" && mv -v "$restextslmpout" "$moddir/res/textslmp.res"
    elif [[ "$lang" == "fra" ]]; then
        local restextsout="${textsfra%_fra}.fra"
        local restextslmpout="${textslmpfra%_fra}.fra"
        wine bin/eipacker.exe /pack "$textsfra" && mv -v "$restextsout" "$moddir/res/texts.res"
        wine bin/eipacker.exe /pack "$textslmpfra" && mv -v "$restextslmpout" "$moddir/res/textslmp.res"
    fi

}

function dds2Mmp {
    local resddsdir="res-dds"
    local texturesddsdir="$resddsdir/textures_res_dds"
    local redressddsdir="$resddsdir/redress_res_dds"
    local texturesmmpdir="$rextdir/textures_res"
    local redressmmpdir="$rextdir/redress_res"

    if [[ "$redressnswr" == "y" ]]; then
        echo "======================================== PROCESSING REDRESS DDS FILES ========================================"
        cd $redressddsdir || exit
        for redressdds in *.dds; do
            local redressmmpout="${redressdds%dds}mmp"
            wine ../../bin/MMPS.exe "$redressdds" && mv -fv "$redressmmpout" ../../"$redressmmpdir"
        done
        cd ../..
        echo "Processed redress"
    fi

    if [[ "$texturesnswr" == "y" ]]; then
        echo "======================================== PROCESSING TEXTURES DDS FILES ========================================"
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
    local resversionname="$reslangdir/texts_res_$lang/string version_name"
    local versiontemplate="version/version-name-format.txt"
    local configini="$inidir/config.ini"
    echo "======================================== WRITING VERSION ========================================"

    if [[ "$writeversionnswr" == "y" ]]; then
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

    sed -i "s/^Version=.*/Version=$version/" "$configini" && echo "Version written in $configini"
    cp -vL "$versiontemplate" "$resversionname"
    sed -i "s/ver\./ver. $version/" "$resversionname" && echo "Version written in $resversionname"
    sed -i "s/commit\./commit. $commithashshort/" "$resversionname" && echo "Commit hash written in $resversionname "


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
    if [[ "$replaceoldnswr" != "n" ]]; then
        rsync -r "$moddir"/ ../../Universal-Mod
        echo "FILES MOVED TO MOD RELEASE DIRECTORY"
    fi
}

# main function
function main {
    checkCommands
    directoryCreation
    copyMaps
    copyHdPack
    eiDbEditor
    dds2Mmp
    writeVersion
    packLanguageTexts
    eiPacker
    ini2Reg
    addLua
    replaceOldMod
}

echo "Welcome to the Evil Islands Auto-Compiler script for GNU/Linux!"
echo "The options in caps are the defaults, options must be written in lowercase."
read -rp "Select the target language for the mod (ENG/fra): " lang

if [[ ! $lang =~ ^(eng|fra)$ ]]; then
    echo "Unsupported language or empty string detected, defaulting to English."
    lang="eng"
fi
moddir="$moddir-$lang"

read -rp "Convert REDRESS from DDS to MMP? (y/N) " redressnswr
read -rp "Convert TEXTURES from DDS to MMP? (y/N) " texturesnswr
read -rp "Increment version? (y/N) " writeversionnswr
read -rp "Replace the older mod files? (Y/n) " replaceoldnswr
echo ""

main

echo ""
echo "DONE PROCESSING $moddir | MOD VERSION $version | COMMIT HASH $commithashshort"
echo ""
