#!/bin/bash

# disable WINE debug messages
export WINEDEBUG=-all

# mod folder location and naming
modout="mods-out"
modfolder="$modout"/"$(date +"%Y-%m-%d_%H-%M")"
# ressources directories
inidir="ini"
regdir="reg"
# ddsdir="dds"
# mmpdir="mmp"
xlsxdir="xlsx"
resdir="res"
rextdir="res-unpacked"
luadir="lua"
# total files
totalreg=""
# totalmmp=""
totalres=""

function directoryCreation {
    echo "Creating mod directory : $modfolder"
    mkdir -p "$modfolder"/config
    mkdir "$modfolder"/res
    mkdir "$modfolder"/maps
}

function ini2Reg {
    echo ""
    echo "======================== PROCESSING INI FILES ========================"
    echo ""
    echo "Converting INI files to REG..."

    for iniin in "$inidir"/*.ini; do
        regout="${iniin%ini}reg"
        wine bin/ini2reg.exe "$iniin"
        mv -fv "$regout" $regdir/
        totalreg="$totalreg $iniin"
    done

    echo "Processed the following files : $totalreg"
    echo "Copying REG files into their respective folder..."
    echo "Files detected in $regdir : $(ls $regdir | xargs)"
    
    mv -v "$regdir"/config.reg "$modfolder" 2>/dev/null
    mv -v "$regdir"/ai.reg "$modfolder"/config 2>/dev/null
    mv -v "$regdir"/music.reg "$modfolder"/config 2>/dev/null
    mv -v "$regdir"/streamsn.reg "$modfolder"/config 2>/dev/null
    mv -v "$regdir"/smessbase.reg "$modfolder"/res 2>/dev/null
    mv -v "$regdir"/autorunpro.reg "$modfolder" 2>/dev/null
    
    echo ""
    echo "=============================== INI FILES PROCESSED ==============================="
    echo ""
}

function dds2MMP {
    echo ""
    echo "=============================== PROCESSING DDS FILES ==============================="
    echo ""
    echo "Converting DDS files to MMP..."

    cd $ddsdir || exit

    for ddsin in *.dds; do
        mmpout="${ddsin%dds}mmp"
        wine ../bin/MMPS.exe "$ddsin"
        mv -fv "$mmpout" ../$mmpdir/
        totalmmp="$totalmmp $ddsin"
    done

    echo "Processed the following files : $totalmmp"
    cd .. || exit
    echo "Moving MMP files to textures_res"
    mkdir "$rextdir"/textures_res 2>/dev/null || echo "textures_res already exists, overwriting..."
    mv -fv "$mmpdir"/* $rextdir/textures_res/

    echo ""
    echo "=============================== DDS FILES PROCESSED ==============================="
    echo ""
}

function eiDBEditor {
    echo ""
    echo "=============================== PROCESSING DATABASELMP ==============================="
    echo ""

    cd $xlsxdir || exit
    echo "Converting XLSX databaselmp to RES..."
    wine start /wait ../bin/eidbeditor-144/DBEditor.exe databaselmp.xlsx
    mv -fv ../"$xlsxdir"/databaselmp.res ../"$resdir"/
    cd .. || exit

    echo ""
    echo "=============================== DATABASELMP PROCESSED ==============================="
    echo ""
}

function writeCommitIntoVersion {
    commithashshort="$(git rev-parse --short HEAD)"
    versionfile="modversion.txt"
    resversionname="./res-unpacked/texts_res/string version_name"

    echo ""
    echo "======================== WRITING VERSION HASH ========================"
    echo ""

    cp -v "$versionfile" "$resversionname"
    sed -i "s/ver\./ver. $commithashshort/" "$resversionname"
    echo "File $resversionname updated with commit hash: $commithashshort"

    echo ""
    echo "======================== VERSION HASH WRITTEN ========================"
    echo ""
}

function eiPacker {
    echo ""
    echo "=============================== PROCESSING RES FILES ==============================="
    echo ""
    echo "Packing RES files..."

    for rextin in "$rextdir"/*_res; do
        resout="${rextin%_res}.res"
        wine bin/eipacker.exe /pack "$rextin"
        rsync -r --remove-source-files "$resout" "$resdir"/
        echo "RSync completed on $resout"
        totalres="$totalres $rextin"
    done

    find ./"$rextin" -depth -type d -empty -delete
    echo "Empty directories deleted"

    echo "Moving RES files to $modfolder/res/"
    mv -fv "$resdir"/*.res "$modfolder"/res/

    echo ""
    echo "=============================== RES FILES PROCESSED ==============================="
    echo ""
}

function addLua {
    echo ""
    echo "=============================== ADDING LUA SCRIPTS ==============================="
    echo ""

    cp -v "$luadir"/main.lua "$modfolder"/
    cp -vrL "$luadir"/lua "$modfolder"/lua

    echo ""
    echo "=============================== LUA SCRIPTS ADDED ==============================="
    echo ""
}

# CALLING FUNCTIONS IN THE RIGHT ORDER
directoryCreation
ini2Reg
#dds2MMP // function declared but disabled
eiDBEditor
writeCommitIntoVersion
eiPacker
addLua


echo "DONE PROCESSING $modfolder"