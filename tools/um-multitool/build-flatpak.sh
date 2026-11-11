#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Kyr4l
# Packs um-multitool as a Flatpak bundle (make flatpak): a single .flatpak file, installed with
#   flatpak install --user um-multitool-x86_64.flatpak
# and started from the menu or with `flatpak run io.github.Kyr4l.UmMultitool`.
#   build-flatpak.sh <output .flatpak>
# Builds flatpak/io.github.Kyr4l.UmMultitool.yml (the sandbox permissions are listed there) with flatpak-builder,
# or the org.flatpak.Builder app when the command is not installed; the runtime it needs (org.freedesktop.Sdk and
# Platform 25.08) is taken from Flathub when missing. State, build dir and repo go to build/flatpak/.
set -euo pipefail

OUT="$(realpath -m "${1:-um-multitool-x86_64.flatpak}")"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_ID=io.github.Kyr4l.UmMultitool
MANIFEST="$HERE/flatpak/$APP_ID.yml"
STATE="$HERE/build/flatpak"

if command -v flatpak-builder >/dev/null 2>&1; then
    BUILDER=(flatpak-builder)
elif flatpak info org.flatpak.Builder >/dev/null 2>&1; then
    # The app forwards its own XDG_DATA_HOME (~/.var/app/...) to the `flatpak info` it runs on the host, which then
    # misses a --user runtime: point it back at the real one (flatpak run --env cannot override it).
    BUILDER=(flatpak run --command=sh org.flatpak.Builder -c 'XDG_DATA_HOME="$HOME/.local/share" exec flatpak-builder "$@"' sh)
else
    echo "build-flatpak.sh: needs flatpak-builder, or: flatpak install --user flathub org.flatpak.Builder" >&2
    exit 1
fi

DEPS=() # the runtime from Flathub only when it is missing (the builder app cannot always see the remote's refs)
if ! { flatpak info org.freedesktop.Sdk//25.08 && flatpak info org.freedesktop.Platform//25.08; } >/dev/null 2>&1 &&
   flatpak remotes --columns=name | grep -qx flathub; then
    DEPS=(--install-deps-from=flathub)
fi

mkdir -p "$STATE"
"${BUILDER[@]}" --user "${DEPS[@]}" --force-clean --state-dir="$STATE/state" --repo="$STATE/repo" \
    "$STATE/app" "$MANIFEST"
flatpak build-bundle --runtime-repo=https://dl.flathub.org/repo/flathub.flatpakrepo \
    "$STATE/repo" "$OUT.part" "$APP_ID"
mv "$OUT.part" "$OUT"
echo "Built $OUT"
