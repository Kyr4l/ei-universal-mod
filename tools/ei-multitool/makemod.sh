#!/bin/bash

# disable WINE debug messages
export WINEDEBUG=-all

# mod folder location and naming
modout="mods-out"
modfolder="$modout"/"$(date +"%y%m%d-%H%M")"
# resources directories
xlsxdir="xlsx"
resdir="res"
rextdir="res-unpacked"
luadir="lua"
# total res files
totalres=""

function checkCommands {
    for cmd in wine rsync; do
        if ! command -v "$cmd" &>/dev/null; then
            echo "Error : command '$cmd' is required."
            exit 1
        fi
    done
}

function directoryCreation {
    echo "CREATED MOD DIRECTORY: $modfolder"
    echo ""
    mkdir -p "$modfolder"/config
    mkdir "$modfolder"/res
    mkdir "$modfolder"/maps
}

function ini2Reg {
    echo "======================== PROCESSING INI FILES ========================"
    echo "Converting INI files to REG…"

    inidir="ini"
    regdir="reg"

    for iniin in "$inidir"/*.ini; do
        regout="${iniin%ini}reg"
        wine bin/ini2reg.exe "$iniin"
        mv -fv "$regout" "$regdir"
    done

    echo "Copying REG files into their respective folder…"
    echo "Files detected in $regdir : $(find .. -print0 | xargs -0 .. 2>/dev/null)"
    mv -v "$regdir"/config.reg "$modfolder" 2>/dev/null
    mv -v "$regdir"/ai.reg "$modfolder"/config 2>/dev/null
    mv -v "$regdir"/music.reg "$modfolder"/config 2>/dev/null
    mv -v "$regdir"/streamsn.reg "$modfolder"/config 2>/dev/null
    mv -v "$regdir"/smessbase.reg "$modfolder"/res 2>/dev/null
    mv -v "$regdir"/autorunpro.reg "$modfolder" 2>/dev/null
    echo "======================================================================"
    echo ""
}

function dds2Mmp {
    echo "=============================== PROCESSING DDS FILES ==============================="
    resddsdir="res-dds"
    # DDS
    texturesddsdir="$resddsdir/textures_res_dds"
    redressddsdir="$resddsdir/redress_res_dds"
    # MMP
    texturesmmpdir="$rextdir/textures_res"
    redressmmpdir="$rextdir/redress_res"

    read -rp "Convert REDRESS from DDS to MMP ? (y/N) " redressAnswer
    if [[ "$redressAnswer" == "y" ]]; then
        cd $redressddsdir || exit
        for redressdds in *.dds; do
            redressmmpout="${redressdds%dds}mmp"
            wine ../../bin/MMPS.exe "$redressdds"
            mv -fv "$redressmmpout" ../../"$redressmmpdir"
        done
        cd ../..
        echo "Processed redress"
    fi

    read -rp "Convert TEXTURES from DDS to MMP ? (y/N) " texturesAnswer
    if [[ "$texturesAnswer" == "y" ]]; then
        cd $texturesddsdir || exit
        for texturesdds in *.dds; do
            texturesmmpout="${texturesdds%dds}mmp"
            wine ../../bin/MMPS.exe "$texturesdds"
            mv -fv "$texturesmmpout" ../../"$texturesmmpdir"
        done
        cd ../..
        echo "Processed textures"
    fi
    echo "===================================================================================="
    echo ""
}

function eiDBEditor {
    echo "=============================== PROCESSING DATABASELMP ==============================="
    cd $xlsxdir || exit
    echo "Converting XLSX databaselmp to RES..."
    wine start /wait ../bin/eidbeditor-144/DBEditor.exe databaselmp.xlsx
    mv -fv ../"$xlsxdir"/databaselmp.res ../"$resdir"
    cd .. || exit
    echo "======================================================================================"
    echo ""
}

function writeVersion {
    resversionname="res-unpacked/texts_res/string version_name"
    versiontemplate="version/version-name-format.txt"
    versionfile="version/mod-version.txt"
    commithashshort="$(git rev-parse --short HEAD)"
    version=$(cat "$versionfile")
    echo "==================================== WRITING VERSION ===================================="

    read -rp "Increment version ? (y/N) " wvAnswer
    if [[ "$wvAnswer" == "y" ]]; then
        major=$(echo "$version" | cut -d. -f1)
        minor=$(echo "$version" | cut -d. -f2)
        patch=$(echo "$version" | cut -d. -f3)

        if [ "$patch" -lt 9 ]; then
            patch=$((patch + 1))
        else
            patch=0
            if [ "$minor" -lt 9 ]; then
                minor=$((minor + 1))
            else
                minor=0
                major=$((major + 1))
            fi
        fi
        newversion="$major.$minor.$patch"
        version=$newversion
        echo "$newversion" >"$versionfile"
    fi

    cp -v "$versiontemplate" "$resversionname"
    sed -i "s/ver\./ver. $version/" "$resversionname"
    sed -i "s/commit\./commit. $commithashshort/" "$resversionname"

    echo "File $resversionname updated with version $newversion and commit hash $commithashshort"
    echo "========================================================================================="
    echo ""
}

function eiPacker {
    echo "=============================== PACKING RES FILES ==============================="

    for rextin in "$rextdir"/*_res; do
        resout="${rextin%_res}.res"
        wine bin/eipacker.exe /pack "$rextin"
        rsync -r --remove-source-files "$resout" "$resdir"
        echo "RSync completed on $resout"
        totalres="$totalres $rextin"
    done

    echo "Deleting empty directories"
    find ./"$rextin" -depth -type d -empty -delete
    echo "Moving RES files into $modfolder/res"
    mv -fv "$resdir"/*.res "$modfolder"/res

    echo "================================================================================="
    echo ""
}

function addLua {
    echo "=============================== ADDING LUA SCRIPTS ==============================="
    cp -v "$luadir"/main.lua "$modfolder"
    cp -vrL "$luadir"/lua "$modfolder"/lua
    echo "=================================================================================="
    echo ""
}

# CALLING FUNCTIONS IN THE RIGHT ORDER
checkCommands
directoryCreation
ini2Reg
dds2Mmp
eiDBEditor
writeVersion
eiPacker
addLua

echo "DONE PROCESSING $modfolder"
echo ""
