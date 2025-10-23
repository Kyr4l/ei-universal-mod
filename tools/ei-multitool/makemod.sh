#!/bin/bash

# disable WINE debug messages
export WINEDEBUG=-all

# mod folder location and naming
moddir="mods-out"/"$(date +"%y%m%d-%H%M")"
# resources directories
resdir="res"
resxdir="res-unpacked"
resxtextsdir="res-texts"
luadir="lua"
inidir="ini"
mprdir="mpr"
mobdir="mob"
#mqdir="mq" # disabled cause the only line using this is disabled as well
mqxdir="mq-unpacked"
hdpackdir="hdlands"
resddsdir="res-dds"
xlsxdir="xlsx"

# stop execution if rsync/wine/progress is missing
function checkCommands {
    for cmd in wine rsync parallel; do
        if ! command -v "$cmd" &>/dev/null; then
            echo "Error: command '$cmd' is required"
            exit 1
        fi
    done
}

# create the mod directory structure
function directoryCreation {
    echo "CREATED MOD DIRECTORY: $moddir"
    mkdir -vp "$moddir"/{config,"res/lang",maps}
}

function ini2Reg {
    echo "======================================== PROCESSING INI FILES ========================================"
    echo "Converting INI files to REG"
    parallel wine bin/ini2reg.exe {} ::: "$inidir"/*.ini

    echo "Copying REG files into their respective folder…"
    mv -v "$inidir"/{config,autorunpro}.reg "$moddir" 2>/dev/null
    mv -v "$inidir"/{ai,music,streamsn}.reg "$moddir"/config 2>/dev/null
    mv -v "$inidir"/smessbase.reg "$moddir"/res 2>/dev/null
}

function makeQuests {
    echo ""======================================== PROCESSING QUEST FILES ========================================""
    # This function does things that other functions already do, but handling quests is slightly more complicated and requires an RM operation to remain clean, therefore it's separated.
    echo "Converting quest INI files to REG"
    find "$mqxdir"/ -type f -name "*.ini" -maxdepth 3 -exec realpath -z {} + | parallel -0 wine bin/eipacker.exe {}

    #echo "Removing INI files before packing" 
    #find "$mqxdir"/ -type f -name "*.ini" -maxdepth 3 -delete -print

    echo "Packing quests"
    parallel wine bin/eipacker.exe {} ::: "$mqxdir"/*

    echo "Converting REG files back to INI"
    find "$mqxdir"/ -type f -name "*.reg" -maxdepth 3 -exec realpath -z {} + | parallel -0 wine bin/reg2ini.exe {}

    echo "Cleaning up REG files"
    find "$mqxdir"/ -type f -name "*.reg" -maxdepth 3 -delete -print

    echo "Moving MQ files to the mod's MAPS directory"
    mv -v "$mqxdir"/*.mq "$moddir/maps"
}

function copyMaps {
    echo "======================================== COPYING MAPS ========================================"
    cp -rv "$mprdir"/* "$moddir/maps"
    cp -rv "$mobdir"/* "$moddir/maps"
    #cp -rv "$mqdir"/* "$moddir/maps" # disabled cause replaced in makeQuests
}

function copyHdPack {
    echo "======================================== COPYING HD PACK ========================================"
    rsync -rv "$hdpackdir"/ "$moddir/hdlands"
}

function packTexts {
    echo "======================================== PACKING TEXTS & TEXTSLMP ========================================"

    for restexts in "$resxtextsdir"/*_res; do
        local packedtexts="${restexts%_res}.res"
        wine bin/eipacker.exe /pack "$restexts" && mv -v "$packedtexts" "$moddir"/res/lang/
    done

    cp -v "$moddir"/res/lang/texts-eng.res "$moddir"/res/texts.res
    cp -v "$moddir"/res/lang/textslmp-eng.res "$moddir"/res/textslmp.res

    echo "Copy the texts-<LANGUAGE>.res and textslmp-<LANGUAGE>.res files from this directory and paste them in the directory above this one, then rename them to texts.res and textslmp.res" > "$moddir"/res/lang/HOWTO-SWITCHLANG.txt
}

function dds2Mmp {
    local texturesddsdir="$resddsdir/textures_res_dds"
    local redressddsdir="$resddsdir/redress_res_dds"
    local texturesmmpdir="$resxdir/textures_res"
    local redressmmpdir="$resxdir/redress_res"

    if [[ "$redressnswr" == "y" ]]; then
        echo "======================================== PROCESSING REDRESS DDS FILES ========================================"
        cd $redressddsdir || exit
        parallel --bar "wine ../../bin/MMPS.exe {} && mv -f {/.}.mmp ../../$redressmmpdir" ::: *.dds
        cd ../..
        echo "Processed redress"
    fi

    if [[ "$texturesnswr" == "y" ]]; then
        echo "======================================== PROCESSING TEXTURES DDS FILES ========================================"
        cd $texturesddsdir || exit
        parallel --bar "wine ../../bin/MMPS.exe {} && mv -f {/.}.mmp ../../$texturesmmpdir" ::: *.dds
        cd ../..
        echo "Processed textures"
    fi
}

function eiDbEditor {
    echo "======================================== PROCESSING DATABASELMP ========================================"
    cd "$xlsxdir" || exit
    echo "Converting XLSX databaselmp to RES..."
    wine start /wait ../bin/eidbeditor-144/DBEditor.exe databaselmp.xlsx && mv -fv databaselmp.res ../"$resdir"/
    cd ..
}

function writeVersion {
    commithashshort="$(git rev-parse --short HEAD)"
    versionfile="version/mod-version.txt"
    version=$(cat "$versionfile")
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

    for textsres in "$resxtextsdir"/texts-*_res; do
        local stringversionname="$textsres/string version_name"

        sed -i "s/^Version=.*/Version=$version/" "$configini" && echo "Version written in $configini"
        cp -vL "$versiontemplate" "$stringversionname"
        sed -i "s/ver\./ver. $version/" "$stringversionname" && echo "Version written in $stringversionname"
        sed -i "s/post-commit\./post-commit. $commithashshort/" "$stringversionname" && echo "Commit hash written in $stringversionname"

        echo "File $stringversionname updated with version $version and commit hash $commithashshort"
    done
}

function packRes {
    echo "======================================== PACKING RES FILES ========================================"

    for resxin in "$resxdir"/*_res; do
        local resout="${resxin%_res}.res"
        wine bin/eipacker.exe /pack "$resxin" && mv -v "$resout" "$resdir"
    done

    echo "Deleting empty directories"
    find ./"$resxin" -depth -type d -empty -delete
    echo "Moving RES files into $moddir/res"
    mv -fv "$resdir"/*.res "$moddir"/res
}

function addLua {
    echo "======================================== ADDING LUA SCRIPTS ========================================"
    cp -vL "$luadir"/main.lua "$moddir"
    cp -vrL "$luadir"/lua "$moddir"/lua
}

function replaceOldMod {
    echo "======================================== REPLACING OLD MOD FILES ========================================"
    if [[ "$replaceoldnswr" != "n" ]]; then
        echo "============================== COPYING FILES TO MOD RELEASE DIRECTORY =============================="
        rsync -rv --exclude "saves" --exclude "mp" --exclude "switchlang.bat" --delete "$moddir"/ ../../Universal-Mod
        echo "FILES MOVED TO MOD RELEASE DIRECTORY"
    fi
}

# function cleanUp {
#     echo "======================================== REMOVING PACKED ASSETS ========================================"
#     if [[ "$cleanupnswr" != "n" ]]; then
#         echo "TODO"
#     fi
# }

# main function
function main {
    checkCommands
    directoryCreation
    ini2Reg
    makeQuests
    copyMaps
    copyHdPack
    eiDbEditor
    dds2Mmp
    writeVersion
    packTexts
    packRes
    addLua
    replaceOldMod
    #cleanUp
}

echo "Welcome to the Evil Islands Auto-Packing script for GNU/Linux!"
echo "The options in caps are the defaults, options must be written in lowercase."

read -rp "Convert REDRESS from DDS to MMP? (y/N) " redressnswr
read -rp "Convert TEXTURES from DDS to MMP? (y/N) " texturesnswr
read -rp "Increment version? (y/N) " writeversionnswr
read -rp "Replace the older mod files? (Y/n) " replaceoldnswr
#read -rp "Cleanup after packing? (Y/n) " cleanupnswr
echo ""

main

echo ""
echo "DONE PROCESSING $moddir | MOD VERSION $version | COMMIT HASH $commithashshort"
echo ""
