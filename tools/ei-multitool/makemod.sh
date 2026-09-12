#!/usr/bin/env bash
# ==============================================================================
# Evil Islands Auto-Packing Script (makemod)
# ==============================================================================
# Automates compiling, packing, and assembling the Evil Islands Universal Mod.
#
# Requirements:
#   - wine (for legacy Windows CLI tools: eipacker, DBEditor, ini2reg, etc.)
#   - rsync
#   - parallel (GNU Parallel)
#   - i686-w64-mingw32-g++ (MinGW 32-bit cross compiler for um.dll)
#   - bin/um-mobdump (C++ MOB dumper tool)
#
# Usage:
#   ./makemod.sh [options]
#
# Options:
#   -i, --interactive       Prompt interactively for all options
#   --redress               Convert REDRESS textures from DDS to MMP
#   --textures              Convert world TEXTURES from DDS to MMP
#   --textures-zones        Convert ZONE TEXTURES from DDS to MMP
#   --dds-all               Convert all DDS textures (redress, textures, zones)
#   --no-mob-dump           Skip dumping MOB files to YAML/EIS
#   -v, --bump-version      Increment semantic patch version in mod-version.txt
#   --no-replace            Skip syncing build output to ../../Universal-Mod
#   --clean-builds          Delete all previous builds in mods-out/ before starting
#   -j, --jobs <N>          Concurrency level for GNU Parallel (default: $(nproc))
#   --completion            Output bash autocompletion script
#   -h, --help              Show this help message
# ==============================================================================

# Note: Do not enable `set -e` as legacy WINE tools and wildcard lookups may return non-zero exit codes

# ------------------------------------------------------------------------------
# Terminal Color Codes & UI Helpers
# ------------------------------------------------------------------------------
if [[ -t 1 ]]; then
    RESTORE=$'\033[0m'
    RED=$'\033[0;31m'
    GREEN=$'\033[0;32m'
    YELLOW=$'\033[0;33m'
    BLUE=$'\033[0;34m'
    CYAN=$'\033[0;36m'
    BOLD=$'\033[1m'
else
    RESTORE="" RED="" GREEN="" YELLOW="" BLUE="" CYAN="" BOLD=""
fi

log_step()  { echo -e "\n${YELLOW}${BOLD}==== [STEP] $* ====${RESTORE}"; }
log_info()  { echo -e "${CYAN}[INFO]${RESTORE} $*"; }
log_ok()    { echo -e "${GREEN}[OK]${RESTORE} $*"; }
log_warn()  { echo -e "${YELLOW}[WARN]${RESTORE} $*"; }
log_error() { echo -e "${RED}[ERROR]${RESTORE} $*" >&2; }

# ------------------------------------------------------------------------------
# Environment & Directory Configuration
# ------------------------------------------------------------------------------
export WINEDEBUG=-all

BUILD_TIMESTAMP="$(date +"%y%m%d-%H%M")"
readonly BUILD_TIMESTAMP
readonly MOD_DIR="mods-out/$BUILD_TIMESTAMP"

# Source directories
readonly RES_DIR="res"
readonly RES_UNPACKED_DIR="res-unpacked"
readonly RES_TEXTS_DIR="res-texts"
readonly INI_DIR="ini"
readonly MPR_DIR="mpr"
readonly MOB_DIR="mob"
readonly MOB_DUMP_DIR="mob-dump"
readonly MQ_UNPACKED_DIR="mq-unpacked"
readonly STREAM_DIR="stream"
readonly MOVIES_DIR="movies"
readonly HD_PACK_DIR="hdlands"
readonly RES_DDS_DIR="res-dds"
readonly XLSX_DIR="xlsx"
readonly XLSX_DUMP_DIR="xlsx-dump"
readonly VERSION_FILE="version/mod-version.txt"
readonly VERSION_TEMPLATE="version/version-name-format.txt"
readonly RELEASE_TARGET_DIR="../../Universal-Mod"

# Default configuration flags
OPT_INTERACTIVE=false
OPT_REDRESS=false
OPT_TEXTURES=false
OPT_TEXTURES_ZONES=false
OPT_DUMP_MOB=true
OPT_BUMP_VERSION=false
OPT_REPLACE_RELEASE=true
OPT_CLEAN_BUILDS=false
PARALLEL_JOBS="$(nproc 2>/dev/null || echo 4)"
PREV_BUILD_DIR=""
BUILD_START_TIME=0

# ------------------------------------------------------------------------------
# Usage & Help
# ------------------------------------------------------------------------------
print_help() {
    cat <<EOF
${BOLD}Evil Islands Auto-Packing Tool (makemod)${RESTORE}

${BOLD}USAGE:${RESTORE}
    ./makemod.sh [OPTIONS]

${BOLD}OPTIONS:${RESTORE}
    -i, --interactive       Prompt interactively before starting build
    --redress               Convert REDRESS textures from DDS to MMP
    --textures              Convert general TEXTURES from DDS to MMP
    --textures-zones        Convert ZONE TEXTURES from DDS to MMP
    --dds-all               Enable all DDS texture conversions
    --no-mob-dump           Disable dumping MOB files to YAML and EIS
    -v, --bump-version      Increment semantic version (X.Y.Z -> X.Y.Z+1)
    --no-replace            Do not sync build to ../../Universal-Mod
    --clean-builds          Delete all previous builds in mods-out/ before starting
    -j, --jobs <N>          Number of parallel worker processes (default: ${PARALLEL_JOBS})
    -h, --help              Print this help documentation

${BOLD}EXAMPLES:${RESTORE}
    ./makemod.sh                     # Fast build using defaults
    ./makemod.sh -i                  # Interactive mode
    ./makemod.sh --clean-builds      # Clean old builds and create a fresh build
    ./makemod.sh --dds-all           # Convert all DDS textures & build
    ./makemod.sh --bump-version      # Bump version & build mod
EOF
}

# ------------------------------------------------------------------------------
# CLI Option Parsing
# ------------------------------------------------------------------------------
parse_cli_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            -i|--interactive)
                OPT_INTERACTIVE=true
                shift
                ;;
            --redress)
                OPT_REDRESS=true
                shift
                ;;
            --textures)
                OPT_TEXTURES=true
                shift
                ;;
            --textures-zones)
                OPT_TEXTURES_ZONES=true
                shift
                ;;
            --dds-all)
                OPT_REDRESS=true
                OPT_TEXTURES=true
                OPT_TEXTURES_ZONES=true
                shift
                ;;
            --no-mob-dump)
                OPT_DUMP_MOB=false
                shift
                ;;
            -v|--bump-version)
                OPT_BUMP_VERSION=true
                shift
                ;;
            --no-replace)
                OPT_REPLACE_RELEASE=false
                shift
                ;;
            --clean-builds)
                OPT_CLEAN_BUILDS=true
                shift
                ;;
            -j|--jobs)
                if [[ $# -lt 2 || ! "$2" =~ ^[0-9]+$ ]]; then
                    log_error "Option '$1' requires a numeric argument."
                    exit 1
                fi
                PARALLEL_JOBS="$2"
                shift 2
                ;;
            -h|--help)
                print_help
                exit 0
                ;;
            *)
                log_error "Unknown option: $1"
                echo "Run './makemod.sh --help' for usage."
                exit 1
                ;;
        esac
    done
}

# ------------------------------------------------------------------------------
# Interactive Prompts
# ------------------------------------------------------------------------------
run_interactive_prompts() {
    echo -e "${YELLOW}Interactive Mode Enabled. Press Enter to select defaults [in brackets].${RESTORE}\n"

    read -rp "Convert REDRESS from DDS to MMP? [y/N]: " ans
    [[ "${ans,,}" =~ ^(y|yes)$ ]] && OPT_REDRESS=true || OPT_REDRESS=false

    read -rp "Convert TEXTURES from DDS to MMP? [y/N]: " ans
    [[ "${ans,,}" =~ ^(y|yes)$ ]] && OPT_TEXTURES=true || OPT_TEXTURES=false

    read -rp "Convert TEXTURES-ZONES from DDS to MMP? [y/N]: " ans
    [[ "${ans,,}" =~ ^(y|yes)$ ]] && OPT_TEXTURES_ZONES=true || OPT_TEXTURES_ZONES=false

    read -rp "Dump MOB files for git version tracking? [Y/n]: " ans
    [[ "${ans,,}" =~ ^(n|no)$ ]] && OPT_DUMP_MOB=false || OPT_DUMP_MOB=true

    read -rp "Increment semantic version number? [y/N]: " ans
    [[ "${ans,,}" =~ ^(y|yes)$ ]] && OPT_BUMP_VERSION=true || OPT_BUMP_VERSION=false

    read -rp "Copy built files to release directory ($RELEASE_TARGET_DIR)? [Y/n]: " ans
    [[ "${ans,,}" =~ ^(n|no)$ ]] && OPT_REPLACE_RELEASE=false || OPT_REPLACE_RELEASE=true
}

# ------------------------------------------------------------------------------
# Dependency Verification
# ------------------------------------------------------------------------------
check_dependencies() {
    local missing=()
    for cmd in wine rsync parallel i686-w64-mingw32-g++; do
        if ! command -v "$cmd" &>/dev/null; then
            missing+=("$cmd")
        fi
    done

    if [[ ${#missing[@]} -gt 0 ]]; then
        log_error "Missing required system dependencies: ${missing[*]}"
        exit 1
    fi

    if [[ ! -x "bin/um-mobdump" && ! -f "bin/um-mobdump" ]]; then
        log_warn "bin/um-mobdump not found or not executable. Checking ../um-mobdumper..."
        if [[ -f "../um-mobdumper/um-mobdump" ]]; then
            ln -sf "../um-mobdumper/um-mobdump" "bin/um-mobdump"
            log_ok "Symlinked bin/um-mobdump"
        fi
    fi
}

# ------------------------------------------------------------------------------
# Build Pipeline Steps
# ------------------------------------------------------------------------------

clean_previous_builds() {
    if [[ "$OPT_CLEAN_BUILDS" == true ]]; then
        log_step "Cleaning Previous Builds in mods-out/"
        local cleaned_count=0
        for bdir in mods-out/*/; do
            if [[ -d "$bdir" ]]; then
                log_info "Removing old build: $bdir"
                rm -rf "$bdir"
                ((cleaned_count++)) || true
            fi
        done
        if ((cleaned_count > 0)); then
            log_ok "Removed $cleaned_count previous build(s) to free space"
        else
            log_info "No previous builds found to clean"
        fi
    fi
}

detect_previous_build() {
    # Find the most recent directory in mods-out (excluding current MOD_DIR)
    local latest=""
    for bdir in $(ls -dt mods-out/*/ 2>/dev/null || true); do
        if [[ -d "$bdir" && "$bdir" != "$MOD_DIR/" && "$bdir" != "$MOD_DIR" ]]; then
            latest="$bdir"
            break
        fi
    done
    PREV_BUILD_DIR="$latest"
}

create_directory_structure() {
    log_step "Creating Mod Output Directory: $MOD_DIR"
    mkdir -p "$MOD_DIR"/{config,res,maps,stream,hdlands,movies,lang-packs}
    log_ok "Mod folder initialized"
}

convert_ini_to_reg() {
    log_step "Processing INI Configuration Files"

    # Copy raw INIs
    cp -fv "$INI_DIR"/lightsjigran.ini "$MOD_DIR/config/" 2>/dev/null || true
    cp -fv "$INI_DIR"/lightscavejigran.ini "$MOD_DIR/config/" 2>/dev/null || true
    cp -fv "$INI_DIR/SPELLADDON.INI" "$MOD_DIR/" 2>/dev/null || true

    log_info "Converting INI files to REG..."
    local inis_to_convert=()
    for name in config autorunpro ai music streamsn smessbase; do
        if [[ -f "$INI_DIR/$name.ini" ]]; then
            inis_to_convert+=("$INI_DIR/$name.ini")
        fi
    done

    if [[ ${#inis_to_convert[@]} -gt 0 ]]; then
        parallel -j "$PARALLEL_JOBS" --bar wine bin/ini2reg.exe {} ::: "${inis_to_convert[@]}" > /dev/null || true
    fi

    log_info "Moving converted REG files into mod hierarchy..."
    [[ -f "$INI_DIR/config.reg" ]] && mv -fv "$INI_DIR/config.reg" "$MOD_DIR/" 2>/dev/null || true
    [[ -f "$INI_DIR/autorunpro.reg" ]] && mv -fv "$INI_DIR/autorunpro.reg" "$MOD_DIR/" 2>/dev/null || true
    [[ -f "$INI_DIR/ai.reg" ]] && mv -fv "$INI_DIR/ai.reg" "$MOD_DIR/config/" 2>/dev/null || true
    [[ -f "$INI_DIR/music.reg" ]] && mv -fv "$INI_DIR/music.reg" "$MOD_DIR/config/" 2>/dev/null || true
    [[ -f "$INI_DIR/streamsn.reg" ]] && mv -fv "$INI_DIR/streamsn.reg" "$MOD_DIR/config/" 2>/dev/null || true
    [[ -f "$INI_DIR/smessbase.reg" ]] && mv -fv "$INI_DIR/smessbase.reg" "$MOD_DIR/res/" 2>/dev/null || true
    log_ok "INI & REG files processed"
}

process_quests() {
    log_step "Processing Quest Files (MQ)"
    shopt -s nullglob

    local quest_dirs=("$MQ_UNPACKED_DIR"/mq-*)
    for qdir in "${quest_dirs[@]}"; do
        [[ -d "$qdir" ]] || continue

        local lang_code="${qdir##*/}"
        lang_code="${lang_code#mq-}"
        local lang_pack_dir="$MOD_DIR/lang-packs/$lang_code"

        log_info "Packaging quests for language: ${BLUE}${lang_code}${RESTORE}"

        # 1. Convert quest INI -> REG
        find "$qdir" -maxdepth 3 -type f -name "*.ini" -print0 | \
            parallel -0 -j "$PARALLEL_JOBS" --bar wine bin/ini2reg.exe {} > /dev/null || true

        # 2. Pack quests using eipacker
        parallel -j "$PARALLEL_JOBS" --bar wine bin/eipacker.exe {} ::: "$qdir"/* > /dev/null || true

        # 3. Convert REG back to INI to keep source clean
        find "$qdir" -maxdepth 3 -type f -name "*.reg" -print0 | \
            parallel -0 -j "$PARALLEL_JOBS" --bar wine bin/reg2ini.exe {} > /dev/null || true

        # 4. Clean up temporary REG files
        find "$qdir" -maxdepth 3 -type f -name "*.reg" -delete 2>/dev/null || true

        # 5. Distribute packed MQ files
        mkdir -p "$lang_pack_dir/maps"
        if [[ "$lang_code" == "eng" ]]; then
            log_info "Deploying English MQ files to primary mod maps directory"
            mv -fv "$qdir"/*.mq "$MOD_DIR/maps/" 2>/dev/null || true
            cp -fv "$MOD_DIR/maps"/*.mq "$lang_pack_dir/maps/" 2>/dev/null || true
        else
            mv -fv "$qdir"/*.mq "$lang_pack_dir/maps/" 2>/dev/null || true
        fi
    done

    shopt -u nullglob
    log_ok "Quests assembled"
}

copy_maps() {
    log_step "Copying Map Assets (MPR & MOB)"
    rsync -rv "$MPR_DIR/" "$MOD_DIR/maps"
    rsync -rv "$MOB_DIR/" "$MOD_DIR/maps"
    log_ok "Maps copied"
}

dump_mob_files() {
    if [[ "$OPT_DUMP_MOB" == true ]]; then
        log_step "Dumping MOB Files for Git Version Tracking"
        bin/um-mobdump -d "$MOB_DIR" -o "$MOB_DUMP_DIR" -m
        log_ok "MOB files dumped to $MOB_DUMP_DIR"
    fi
}

copy_media() {
    log_step "Copying Audio & Video Media"
    rsync -rv "$STREAM_DIR/" "$MOD_DIR/stream"
    rsync -rv "$MOVIES_DIR/" "$MOD_DIR/movies"
    log_ok "Media assets copied"
}

copy_hd_pack() {
    log_step "Copying HD Landscape Textures"
    rsync -rv "$HD_PACK_DIR/" "$MOD_DIR/hdlands"
    log_ok "HD pack copied"
}

convert_dds_category() {
    local label="$1"
    local src_dir="$2"
    local dest_dir="$3"

    log_info "Converting $label DDS textures to MMP..."
    (
        cd "$src_dir" || exit 1
        parallel -j "$PARALLEL_JOBS" --bar "wine ../../bin/MMPS.exe {} && mv -f {/.}.mmp ../../$dest_dir" ::: *.dds > /dev/null
    )
    log_ok "Converted $label"
}

process_dds_textures() {
    if [[ "$OPT_REDRESS" == true || "$OPT_TEXTURES" == true || "$OPT_TEXTURES_ZONES" == true ]]; then
        log_step "Processing DDS Texture Conversions"

        if [[ "$OPT_REDRESS" == true ]]; then
            convert_dds_category "Redress" "$RES_DDS_DIR/redress_res_dds" "$RES_UNPACKED_DIR/redress_res"
        fi

        if [[ "$OPT_TEXTURES" == true ]]; then
            convert_dds_category "Textures" "$RES_DDS_DIR/textures_res_dds" "$RES_UNPACKED_DIR/textures_res"
        fi

        if [[ "$OPT_TEXTURES_ZONES" == true ]]; then
            convert_dds_category "Textures-Zones" "$RES_DDS_DIR/textures-zones_res_dds" "$RES_UNPACKED_DIR/textures-zones_res"
        fi
    fi
}

process_databases() {
    log_step "Compiling Databases (database.res & databaselmp.res)"
    (
        cd "$XLSX_DIR" || exit 1

        log_info "Converting XLSX database -> RES..."
        wine start /wait ../bin/eidbeditor-144/DBEditor.exe database.xlsx
        log_info "Dumping database.xlsx -> Markdown..."
        python3 ../bin/xlsx2md.py database.xlsx "../$XLSX_DUMP_DIR/database.md"

        log_info "Converting XLSX databaselmp -> RES..."
        wine start /wait ../bin/eidbeditor-144/DBEditor.exe databaselmp.xlsx
        log_info "Dumping databaselmp.xlsx -> Markdown..."
        python3 ../bin/xlsx2md.py databaselmp.xlsx "../$XLSX_DUMP_DIR/databaselmp.md"

        mv -fv database.res "../$RES_DIR/"
        mv -fv databaselmp.res "../$RES_DIR/"
    )
    log_ok "Databases compiled and dumped"
}

update_version_info() {
    log_step "Updating Version & Git Metadata"
    local commit_hash
    commit_hash="$(git rev-parse --short HEAD 2>/dev/null || echo "unknown")"
    local current_version
    current_version="$(cat "$VERSION_FILE")"

    if [[ "$OPT_BUMP_VERSION" == true ]]; then
        IFS='.' read -r major minor patch <<<"$current_version"
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
        current_version="$major.$minor.$patch"
        echo "$current_version" > "$VERSION_FILE"
        log_info "Bumped version to: ${BOLD}$current_version${RESTORE}"
    else
        log_info "Keeping current version: ${BOLD}$current_version${RESTORE}"
    fi

    # Update inidir/config.ini version
    sed -i "s/^Version=.*/Version=$current_version/" "$INI_DIR/config.ini" 2>/dev/null || true

    # Update version_name in texts folders
    for textsres in "$RES_TEXTS_DIR"/texts-*_res; do
        local stringversionname="$textsres/string version_name"
        cp -fvL "$VERSION_TEMPLATE" "$stringversionname"
        sed -i "s/ver\./ver. $current_version/" "$stringversionname"
        sed -i "s/post-commit\./post-commit. $commit_hash/" "$stringversionname"
    done
    log_ok "Version $current_version (commit $commit_hash) written across configs"
}

pack_texts_resources() {
    log_step "Packing Language Text Archives (texts.res & textslmp.res)"

    for restexts in "$RES_TEXTS_DIR"/*_res; do
        [[ -d "$restexts" ]] || continue

        local restextsname="${restexts##*/}"
        local langcode=""
        local targetname=""

        case "$restextsname" in
            texts-*_res)
                langcode="${restextsname#texts-}"
                langcode="${langcode%_res}"
                targetname="texts.res"
                ;;
            textslmp-*_res)
                langcode="${restextsname#textslmp-}"
                langcode="${langcode%_res}"
                targetname="textslmp.res"
                ;;
            *)
                continue
                ;;
        esac

        log_info "Packing $restextsname -> $targetname for $langcode"
        wine bin/eipacker.exe /pack "$restexts"

        local packedfile="${restexts%_res}.res"
        local langpackdir="$MOD_DIR/lang-packs/$langcode/res"
        mkdir -p "$langpackdir"

        if [[ -f "$packedfile" ]]; then
            mv -f "$packedfile" "$langpackdir/$targetname"
        fi

        # Primary English language is copied directly to mod root res
        if [[ "$langcode" == "eng" ]]; then
            mkdir -p "$MOD_DIR/res"
            cp -f "$langpackdir/$targetname" "$MOD_DIR/res/$targetname"
        fi
    done

    echo "Copy texts.res and textslmp.res from lang-packs/<LANGUAGE>/res/ into mod res/ to switch language" > \
        "$MOD_DIR/lang-packs/HOWTO-SWITCHLANG.txt"

    log_ok "Text archives packed"
}

pack_general_resources() {
    log_step "Packing General Resource Archives (res-unpacked -> res)"

    for resxin in "$RES_UNPACKED_DIR"/*_res; do
        [[ -d "$resxin" ]] || continue
        local resout="${resxin%_res}.res"
        log_info "Packing ${resxin##*/}..."
        wine bin/eipacker.exe /pack "$resxin" && mv -fv "$resout" "$RES_DIR/"
    done

    log_info "Moving all RES archives into $MOD_DIR/res/..."
    mv -fv "$RES_DIR"/*.res "$MOD_DIR/res/"
    log_ok "Resource archives deployed"
}

compile_mod_dll() {
    log_step "Compiling Universal Mod DLL (um.dll)"
    i686-w64-mingw32-g++ -shared -o "$MOD_DIR/um.dll" um.cpp \
        -std=c++17 -O3 -flto -static -s -Wall -Wextra -Wno-unused-parameter
    log_ok "um.dll built successfully"
}

sync_to_release_directory() {
    if [[ "$OPT_REPLACE_RELEASE" == true ]]; then
        log_step "Syncing Built Mod to Release Directory ($RELEASE_TARGET_DIR)"
        rsync --checksum -rv \
            --exclude "saves" \
            --exclude "mp" \
            --exclude "um.cfg" \
            --exclude "um.log" \
            --delete \
            "$MOD_DIR/" "$RELEASE_TARGET_DIR"
        log_ok "Synced to $RELEASE_TARGET_DIR"
    else
        log_info "Skipping release sync (--no-replace active)"
    fi
}

print_build_summary() {
    local cur_ver
    cur_ver="$(cat "$VERSION_FILE" 2>/dev/null || echo "unknown")"
    local cur_hash
    cur_hash="$(git rev-parse --short HEAD 2>/dev/null || echo "unknown")"

    # Calculate build duration
    local end_time
    end_time="$(date +%s)"
    local duration=$((end_time - BUILD_START_TIME))
    local minutes=$((duration / 60))
    local seconds=$((duration % 60))
    local time_str=""
    if ((minutes > 0)); then
        time_str="${minutes}m ${seconds}s (${duration}s)"
    else
        time_str="${seconds}s"
    fi

    # Calculate mod output size and file count
    local mod_size
    mod_size="$(du -sh "$MOD_DIR" 2>/dev/null | awk '{print $1}' || echo "unknown")"
    local file_count
    file_count="$(find "$MOD_DIR" -type f 2>/dev/null | wc -l || echo "unknown")"

    # Check synchronization status
    local sync_status
    if [[ "$OPT_REPLACE_RELEASE" == true ]]; then
        sync_status="${GREEN}Synced to $RELEASE_TARGET_DIR${RESTORE}"
    else
        sync_status="${YELLOW}Skipped (--no-replace)${RESTORE}"
    fi

    # Detect new/removed files compared to previous build
    local new_files=()
    local removed_files=()
    if [[ -n "$PREV_BUILD_DIR" && -d "$PREV_BUILD_DIR" ]]; then
        local prev_list
        prev_list="$(cd "$PREV_BUILD_DIR" && find . -type f | sort)"
        local curr_list
        curr_list="$(cd "$MOD_DIR" && find . -type f | sort)"

        while IFS= read -r f; do
            [[ -n "$f" ]] && new_files+=("${f#./}")
        done < <(comm -13 <(echo "$prev_list") <(echo "$curr_list"))

        while IFS= read -r f; do
            [[ -n "$f" ]] && removed_files+=("${f#./}")
        done < <(comm -23 <(echo "$prev_list") <(echo "$curr_list"))
    fi

    echo -e "\n${GREEN}${BOLD}========================================================================${RESTORE}"
    echo -e "${GREEN}${BOLD}  BUILD COMPLETED SUCCESSFULLY!${RESTORE}"
    echo -e "${GREEN}${BOLD}========================================================================${RESTORE}"
    echo -e "  ${BOLD}Mod Output:${RESTORE}      $MOD_DIR"
    echo -e "  ${BOLD}Mod Version:${RESTORE}     $cur_ver"
    echo -e "  ${BOLD}Commit Hash:${RESTORE}     $cur_hash"
    echo -e "  ${BOLD}Build Time:${RESTORE}      $time_str"
    echo -e "  ${BOLD}Total Size:${RESTORE}      $mod_size"
    echo -e "  ${BOLD}Total Files:${RESTORE}     $file_count files"
    echo -e "  ${BOLD}Release Sync:${RESTORE}    $sync_status"
    echo -e "  ${BOLD}Parallel Jobs:${RESTORE}   $PARALLEL_JOBS threads"

    if [[ -n "$PREV_BUILD_DIR" && -d "$PREV_BUILD_DIR" ]]; then
        echo -e "  ${BOLD}Compared With:${RESTORE}   ${PREV_BUILD_DIR%/}"
        if [[ ${#new_files[@]} -eq 0 && ${#removed_files[@]} -eq 0 ]]; then
            echo -e "  ${BOLD}File Changes:${RESTORE}    ${CYAN}Identical file structure (0 new, 0 removed)${RESTORE}"
        else
            echo -e "  ${BOLD}File Changes:${RESTORE}    ${GREEN}+${#new_files[@]} new file(s)${RESTORE}, ${RED}-${#removed_files[@]} removed file(s)${RESTORE}"
            if [[ ${#new_files[@]} -gt 0 && ${#new_files[@]} -le 10 ]]; then
                for nf in "${new_files[@]}"; do
                    echo -e "    ${GREEN}+ [NEW]${RESTORE} $nf"
                done
            elif [[ ${#new_files[@]} -gt 10 ]]; then
                for nf in "${new_files[@]:0:8}"; do
                    echo -e "    ${GREEN}+ [NEW]${RESTORE} $nf"
                done
                echo -e "    ${CYAN}... and $((${#new_files[@]} - 8)) more${RESTORE}"
            fi

            if [[ ${#removed_files[@]} -gt 0 && ${#removed_files[@]} -le 10 ]]; then
                for rf in "${removed_files[@]}"; do
                    echo -e "    ${RED}- [DEL]${RESTORE} $rf"
                done
            elif [[ ${#removed_files[@]} -gt 10 ]]; then
                for rf in "${removed_files[@]:0:8}"; do
                    echo -e "    ${RED}- [DEL]${RESTORE} $rf"
                done
                echo -e "    ${CYAN}... and $((${#removed_files[@]} - 8)) more${RESTORE}"
            fi
        fi
    else
        echo -e "  ${BOLD}Compared With:${RESTORE}   ${CYAN}Initial / clean build (no previous build to compare)${RESTORE}"
    fi

    echo -e "${GREEN}${BOLD}========================================================================${RESTORE}\n"
}

# ------------------------------------------------------------------------------
# Main Execution Entry Point
# ------------------------------------------------------------------------------
main() {
    BUILD_START_TIME="$(date +%s)"
    parse_cli_args "$@"

    if [[ "$OPT_INTERACTIVE" == true ]]; then
        run_interactive_prompts
    fi

    check_dependencies
    detect_previous_build
    clean_previous_builds

    create_directory_structure
    convert_ini_to_reg
    process_quests
    copy_maps
    dump_mob_files
    copy_media
    copy_hd_pack
    process_dds_textures
    process_databases
    update_version_info
    pack_texts_resources
    pack_general_resources
    compile_mod_dll
    sync_to_release_directory

    print_build_summary
}

main "$@"
