#!/bin/bash

# disable WINE debug messages
export WINEDEBUG=-all

# mod folder location and naming
modout="mods-out"
modfolder="$modout"/"$(date +"%Y-%m-%d_%H-%M")"
# resources directories
inidir="ini"
regdir="reg"

# dds directories
ddsdir="dds"
texturesddsdir="$ddsdir/textures_res_dds"
texturesmmpdir="$modfolder/textures_dds"
redressddsdir="$ddsdir/redress_res_dds"
redressmmpdir="$modfolder/redress_dds"

xlsxdir="xlsx"
resdir="res"
rextdir="res-unpacked"
luadir="lua"
# total files
totalreg=""
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
    echo "Converting INI files to REG..."

    for iniin in "$inidir"/*.ini; do
        regout="${iniin%ini}reg"
        wine bin/ini2reg.exe "$iniin"
        mv -fv "$regout" $regdir/
        totalreg="$totalreg $iniin"
    done

    echo "Processed the following files : $totalreg"
    echo "Copying REG files into their respective folder..."
    echo "Files detected in $regdir : $(find .. -print0 | xargs -0 ..)"

    mv -v "$regdir"/config.reg "$modfolder" 2>/dev/null
    mv -v "$regdir"/ai.reg "$modfolder"/config 2>/dev/null
    mv -v "$regdir"/music.reg "$modfolder"/config 2>/dev/null
    mv -v "$regdir"/streamsn.reg "$modfolder"/config 2>/dev/null
    mv -v "$regdir"/smessbase.reg "$modfolder"/res 2>/dev/null
    mv -v "$regdir"/autorunpro.reg "$modfolder" 2>/dev/null

    echo "======================================================================"
    echo ""
}

function dds2MMP {
    echo ""
    echo "=============================== PROCESSING DDS FILES ==============================="
    echo "Converting DDS files to MMP..."

    cd "$ddsdir" || exit

    read -rp "Pack REDRESS DDS to MMP ? (y/N) " redressAnswer
    if [[ "$redressAnswer" == "y" ]]; then
        cd "$redressddsdir" || exit
        for redressdds in *.dds; do
            redressmmpout="${redressdds%dds}mmp"
            wine ../../bin/MMPS.exe "$redressdds"
            mv -fv "$redressmmpout" ../../"$redressmmpdir"/
            totalredressmmp="$totalredressmmp $redressdds"
        done
        echo "Processed the following files : $totalredressmmp"
        cd ..
    fi

    read -rp "Pack TEXTURES DDS to MMP ? (y/N) " texturesAnswer
    if [[ "$texturesAnswer" == "y" ]]; then
        cd "$texturesddsdir" || exit
        for texturesdds in *.dds; do
            texturesmmpout="${texturesdds%dds}mmp"
            wine ../../bin/MMPS.exe "$texturesdds"
            mv -fv "$texturesmmpout" ../../"$texturesmmpdir"/
            totalTexturesmmp="$totalTexturesmmp $texturesdds"
        done
        echo "Processed the following files : $totalTexturesmmp"
        cd ..
    fi

    cd ..

    echo "===================================================================================="
    echo ""
}

function eiDBEditor {
    echo ""
    echo "=============================== PROCESSING DATABASELMP ==============================="

    cd $xlsxdir || exit
    echo "Converting XLSX databaselmp to RES..."
    wine start /wait ../bin/eidbeditor-144/DBEditor.exe databaselmp.xlsx
    mv -fv ../"$xlsxdir"/databaselmp.res ../"$resdir"/
    cd .. || exit

    echo "======================================================================================"
    echo ""
}

function writeVersion {
    resversionname="./res-unpacked/texts_res/string version_name"
    versiontemplate="./version/version-name-format.txt"
    versionfile="./version/mod-version.txt"

    commithashshort="$(git rev-parse --short HEAD)"
    version=$(cat "$versionfile")

    echo ""
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
        echo "$newversion" > "$versionfile"
    fi

    cp -v "$versiontemplate" "$resversionname"

    sed -i "s/ver\./ver. $version/" "$resversionname"
    sed -i "s/commit\./commit. $commithashshort/" "$resversionname"
    
    echo "File $resversionname updated with version $newversion and commit hash $commithashshort"

    echo "========================================================================================="
    echo ""
}

function eiPacker {
    echo ""
    echo "=============================== PROCESSING RES FILES ==============================="
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

    echo "===================================================================================="
    echo ""
}

function addLua {
    echo ""
    echo "=============================== ADDING LUA SCRIPTS ==============================="

    cp -v "$luadir"/main.lua "$modfolder"/
    cp -vrL "$luadir"/lua "$modfolder"/lua

    echo "=================================================================================="
    echo ""
}

# CALLING FUNCTIONS IN THE RIGHT ORDER
directoryCreation
ini2Reg
dds2MMP // function declared but disabled
eiDBEditor
writeVersion
eiPacker
addLua

echo "DONE PROCESSING $modfolder"
