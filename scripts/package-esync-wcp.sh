#!/bin/bash
# package-esync-wcp.sh
# Package the ESYNC-enabled Wine 11 build into a .wcp compatible with
# Winlator, WinNative, and any other Android Wine emulator following the
# Proton-type profile format.
#
# Key compatibility details:
#   1. XZ compression (NOT zstd) — matches what Winlator/CMOD/Ludashi expect.
#   2. bin/ contains REAL binaries and REAL shell-script wrappers, NOT
#      symlinks. Wine 11's `make install` leaves bin/* as broken symlinks
#      (pointing to nonexistent bin/wine); we resolve them to:
#         - bin/wine             <- copy of lib/wine/aarch64-unix/wine
#         - bin/wine-preloader   <- copy of lib/wine/aarch64-unix/wine-preloader
#         - bin/winecfg et al.   <- standard Wine wrapper shell script
#      This avoids all symlink-extraction errors on Android scoped storage,
#      FAT32 SD cards, and Android archive extractors.
#   3. Tar entries with NO leading "./" prefix (matches reference format).
#
# Usage:
#   ./package-esync-wcp.sh <input_dir> <output_prefix> <version_name>
# Example:
#   ./package-esync-wcp.sh $HOME/compiled-files-aarch64-esync \
#       /home/max/Build/Emulator/Proton/esync-build/dist/winnative-proton-11.0-esync-$(date +%Y%m%d)-arm64ec \
#       "winnative-proton-11.0-esync-arm64ec"

set -euo pipefail

INPUT_DIR="${1:?input_dir required}"
OUTPUT_PREFIX="${2:?output_prefix required}"
VERSION_NAME="${3:?version_name required}"
VERSION_CODE="${4:-110003}"
DESCRIPTION="${5:-WinNative Proton 11.0 with ESYNC (Wine 11.0 + eventfd sync)}"
# Sixth arg selects the architecture-specific resources. Defaults to arm64ec
# for backwards compatibility with existing invocations.
ARCH="${6:-arm64ec}"

case "$ARCH" in
    arm64ec) WINE_UNIX_DIR="aarch64-unix" ;;
    x86_64)  WINE_UNIX_DIR="x86_64-unix" ;;
    *) echo "ERROR: unsupported arch '$ARCH' (expected arm64ec or x86_64)" >&2; exit 1 ;;
esac

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PREFIX_PACK_SOURCE="$PROJ_ROOT/../work/prefixPack-winnative-${ARCH}.txz"
if [[ ! -f "$PREFIX_PACK_SOURCE" ]]; then
    PREFIX_PACK_SOURCE="$PROJ_ROOT/resources/prefixPack-${ARCH}.txz"
fi

[[ -d "$INPUT_DIR" ]] || { echo "ERROR: input_dir $INPUT_DIR missing"; exit 1; }
for required in bin lib share; do
    [[ -e "$INPUT_DIR/$required" ]] || { echo "ERROR: missing $INPUT_DIR/$required"; exit 1; }
done
[[ -f "$PREFIX_PACK_SOURCE" ]] || { echo "ERROR: prefixPack not found at $PREFIX_PACK_SOURCE"; exit 1; }

STAGING_DIR="$(mktemp -d)"
TAR_TMP="$(mktemp --suffix=.tar)"
trap 'rm -rf "$STAGING_DIR" "$TAR_TMP"' EXIT
cp -a "$INPUT_DIR/." "$STAGING_DIR/"
cp "$PREFIX_PACK_SOURCE" "$STAGING_DIR/prefixPack.txz"

# ---------------------------------------------------------------------------
# Post-process prefixPack: strip winebth artifacts so freshly-created Winlator
# containers never have the baked-in winebth service + PnP node. Eliminates
# the err:ntoskrnl:ZwLoadDriver c00000e5 on every container boot.
#
# Winlator has no BlueZ/dbus → winebth can never load, and having Services\\winebth
# + Enum\\Root\\WINE\\WINEBTH pre-populated in the prefix registry forces SCM
# autostart to fail loudly. Ship the prefix without them.
# ---------------------------------------------------------------------------
PREFIX_TMP="$(mktemp -d)"
tar -xf "$STAGING_DIR/prefixPack.txz" -C "$PREFIX_TMP"

# Remove winebth inf + sys + driverstore files.
rm -f "$PREFIX_TMP/.wine/drive_c/windows/inf/winebth.inf" \
      "$PREFIX_TMP/.wine/drive_c/windows/system32/drivers/winebth.sys"
rm -rf "$PREFIX_TMP/.wine/drive_c/windows/system32/driverstore/filerepository"/winebth*

# Strip winebth registry entries from system.reg. Wine's reg file format is
# `[KeyPath] timestamp` followed by values until blank line. Delete whole
# blocks whose [...] header contains winebth (case-insensitive).
if [[ -f "$PREFIX_TMP/.wine/system.reg" ]]; then
    python3 - "$PREFIX_TMP/.wine/system.reg" <<'PY'
import re, sys, io
path = sys.argv[1]
with open(path, 'r', encoding='utf-8', errors='replace') as f:
    lines = f.readlines()

out = []
i = 0
stripped = 0
while i < len(lines):
    line = lines[i]
    if line.startswith('['):
        # Check header for winebth (case-insensitive). Collect block until blank line or next [.
        header = line
        block_start = i
        j = i + 1
        while j < len(lines) and not lines[j].startswith('['):
            if lines[j].strip() == '' and j + 1 < len(lines) and lines[j+1].startswith('['):
                j += 1
                break
            if lines[j].strip() == '':
                break
            j += 1
        block = lines[block_start:j]
        if re.search(r'winebth', header, re.IGNORECASE):
            stripped += 1
            # Skip trailing blank line too, for tidy output
            while j < len(lines) and lines[j].strip() == '':
                j += 1
        else:
            out.extend(block)
        i = j
    else:
        out.append(line)
        i += 1

with open(path, 'w', encoding='utf-8') as f:
    f.writelines(out)
print(f"stripped {stripped} winebth registry block(s) from system.reg", file=sys.stderr)
PY
fi

# Repack prefixPack.
(cd "$PREFIX_TMP" && tar --owner=0 --group=0 --sort=name -cf - .wine) | xz -T0 -9e -c > "$STAGING_DIR/prefixPack.txz"
rm -rf "$PREFIX_TMP"

# ---------------------------------------------------------------------------
# bin/ flatten: replace symlinks with real files so the archive is portable
# to any extractor (Winlator, WinNative, Android zip tools, FAT32 sdcards).
#
# Wine 11 make install leaves bin/<tool> as symlinks to "wine" (sibling)
# when wine itself is at lib/wine/aarch64-unix/wine. We:
#   1. Copy the real wine + wine-preloader binaries to bin/ (no symlink).
#   2. Replace each bin/<tool> symlink with a real copy of Wine's wrapper
#      shell script. `basename $0` inside the script determines which
#      .exe to load, so a single wrapper template serves all tools.
# ---------------------------------------------------------------------------

WINE_BIN="$STAGING_DIR/lib/wine/$WINE_UNIX_DIR/wine"
WINE_PRELOADER_BIN="$STAGING_DIR/lib/wine/$WINE_UNIX_DIR/wine-preloader"

# Wine 11's launcher (loader/main.c) locates ntdll.so via `/proc/self/exe`
# dirname — it looks for ntdll.so in the SAME directory as the wine binary.
# If we copy the real binary to `bin/wine`, it searches `bin/ntdll.so` and
# fails because ntdll.so lives at `lib/wine/aarch64-unix/ntdll.so`.
#
# Wine 10 had extra path-search logic baked into the launcher ("try ../lib/
# wine/<arch>-unix/ntdll.so"). Wine 11 dropped it.
#
# Fix: make `bin/wine` a thin shell-script wrapper that execs the real
# launcher at `lib/wine/<arch>-unix/wine`. Then `/proc/self/exe` resolves
# to the real binary's location, and ntdll.so is found next to it.
# Same treatment for wine-preloader.
#
# This matches the behavior Winlator / WinNative / compatible emulators expect.

if [[ -f "$WINE_BIN" ]]; then
    rm -f "$STAGING_DIR/bin/wine"
    cat > "$STAGING_DIR/bin/wine" <<'EOF'
#!/bin/sh
#
# Wine 11 launcher wrapper — execs the real loader in lib/wine/<arch>-unix/
# so its /proc/self/exe dirname contains ntdll.so and friends.
#
here="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
if   [ -x "$here/../lib/wine/aarch64-unix/wine" ]; then
    exec "$here/../lib/wine/aarch64-unix/wine" "$@"
elif [ -x "$here/../lib/wine/x86_64-unix/wine" ]; then
    exec "$here/../lib/wine/x86_64-unix/wine" "$@"
elif [ -x "$here/../lib/wine/arm64ec-unix/wine" ]; then
    exec "$here/../lib/wine/arm64ec-unix/wine" "$@"
fi
echo "wine: cannot locate real wine loader under $here/../lib/wine/" >&2
exit 1
EOF
    chmod 755 "$STAGING_DIR/bin/wine"
fi

if [[ -f "$WINE_PRELOADER_BIN" ]]; then
    rm -f "$STAGING_DIR/bin/wine-preloader"
    cat > "$STAGING_DIR/bin/wine-preloader" <<'EOF'
#!/bin/sh
#
# Wine 11 preloader wrapper — execs the real preloader in lib/wine/<arch>-unix/
#
here="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
if   [ -x "$here/../lib/wine/aarch64-unix/wine-preloader" ]; then
    exec "$here/../lib/wine/aarch64-unix/wine-preloader" "$@"
elif [ -x "$here/../lib/wine/x86_64-unix/wine-preloader" ]; then
    exec "$here/../lib/wine/x86_64-unix/wine-preloader" "$@"
elif [ -x "$here/../lib/wine/arm64ec-unix/wine-preloader" ]; then
    exec "$here/../lib/wine/arm64ec-unix/wine-preloader" "$@"
fi
echo "wine-preloader: cannot locate real preloader under $here/../lib/wine/" >&2
exit 1
EOF
    chmod 755 "$STAGING_DIR/bin/wine-preloader"
fi

# Standard Wine wrapper script. This is the exact template Wine's make install
# produces on x86_64 Linux; Wine 11 ARM64 install skips generating it and
# creates symlinks instead, so we write it here.
WINE_WRAPPER='#!/bin/sh
#
# Wrapper script to start a Winelib application once it is installed
#
# Copyright (C) 2002 Alexandre Julliard
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
#

# determine the app Winelib library name
appname=`basename "$0" .exe`.exe

# first try explicit WINELOADER
if [ -x "$WINELOADER" ]; then exec "$WINELOADER" "$appname" "$@"; fi

# now try the directory containing $0
appdir=""
case "$0" in
  */*)
    # $0 contains a path, use it
    appdir=`dirname "$0"`
    ;;
  *)
    # no directory in $0, search in PATH
    saved_ifs=$IFS
    IFS=:
    for d in $PATH
    do
      IFS=$saved_ifs
      if [ -x "$d/$0" ]; then appdir="$d"; break; fi
    done
    ;;
esac
if [ -x "$appdir/wine" ]; then exec "$appdir/wine" "$appname" "$@"; fi
if [ -x "$appdir/wine64" ]; then exec "$appdir/wine64" "$appname" "$@"; fi

# now look in PATH
saved_ifs=$IFS
IFS=:
for d in $PATH
do
  IFS=$saved_ifs
  if [ -x "$d/wine" ]; then exec "$d/wine" "$appname" "$@"; fi
  if [ -x "$d/wine64" ]; then exec "$d/wine64" "$appname" "$@"; fi
done

# finally, the default bin directory
if [ -x "/usr/local/bin/wine" ]; then exec "/usr/local/bin/wine" "$appname" "$@"; fi
if [ -x "/usr/local/bin/wine64" ]; then exec "/usr/local/bin/wine64" "$appname" "$@"; fi

echo "$0: the Wine loader is missing"
exit 1
'

# Replace every symlink in bin/ (except wine itself, which is now a real
# binary) with the wrapper script. Also handle the case where bin/<tool>
# doesn't exist at all (rare) by creating it from the wrapper template.
for tool_path in "$STAGING_DIR/bin"/*; do
    tool_name="$(basename "$tool_path")"
    # wine and wine-preloader are real binaries we just installed; leave them.
    case "$tool_name" in
        wine|wine-preloader|wineserver) continue ;;
    esac
    # For everything else in bin/: if it's a symlink, replace with wrapper.
    if [[ -L "$tool_path" ]]; then
        rm -f "$tool_path"
        printf '%s' "$WINE_WRAPPER" > "$tool_path"
        chmod 755 "$tool_path"
    fi
done

# libandroid-sysvshm.so into lib/. The Wine source tree may live at
# $PROJ_ROOT/wine-source/ (legacy/local layout) or $PROJ_ROOT/ (CI / repo
# layout). Try the repo path first because it's authoritative when both
# exist; fall back to the legacy path. Hard-fail if neither has the .so:
# Wine dlopens it at startup for SysV-SHM emulation, and shipping a .wcp
# without it leaves the prefix init hanging on first launch.
# Pick the sysvshm build directory matching the target arch.
case "$ARCH" in
    arm64ec) SYSVSHM_BUILD_DIR="build-aarch64" ;;
    x86_64)  SYSVSHM_BUILD_DIR="build-x86_64" ;;
esac
SYSVSHM_SRC=""
for _candidate in \
    "$PROJ_ROOT/android/android_sysvshm/$SYSVSHM_BUILD_DIR/libandroid-sysvshm.so" \
    "$PROJ_ROOT/wine-source/android/android_sysvshm/$SYSVSHM_BUILD_DIR/libandroid-sysvshm.so"; do
    if [[ -f "$_candidate" ]]; then
        SYSVSHM_SRC="$_candidate"
        break
    fi
done
if [[ -z "$SYSVSHM_SRC" ]]; then
    echo "ERROR: libandroid-sysvshm.so ($ARCH) not found; build it via" >&2
    case "$ARCH" in
        arm64ec) echo "       'bash build-scripts/build-step-arm64ec.sh --build-sysvshm' first" >&2 ;;
        x86_64)  echo "       'bash build-scripts/build-step-x86_64.sh --build-sysvshm' first"  >&2 ;;
    esac
    exit 1
fi
cp -f "$SYSVSHM_SRC" "$STAGING_DIR/lib/libandroid-sysvshm.so"
echo "bundled libandroid-sysvshm.so from $SYSVSHM_SRC"

# Helper to write profile.json with a given type + versionCode.
write_profile() {
    local out="$1" ptype="$2" vcode="$3"
    python3 - "$out" "$VERSION_NAME" "$vcode" "$DESCRIPTION" "$ptype" <<'PY'
import json, sys
out, ver, code, desc, ptype = sys.argv[1:6]
profile = {
    "type": ptype,
    "versionName": ver,
    "versionCode": int(code),
    "description": desc,
    "files": [],
    "wine": {"binPath": "bin", "libPath": "lib", "prefixPack": "prefixPack.txz"},
}
with open(out, "w") as f:
    json.dump(profile, f, indent=2)
print(f"wrote {out}")
PY
}

# Both archives are identical tar.xz streams; only profile.json differs.
#
#   A. <prefix>.wcp          — type "Proton", versionCode >=1 — for WinNative
#   B. <prefix>.wcp.xz       — type "Proton", versionCode 0   — for Winlator / Ludashi (patched)
#
# History note: earlier Winlator/Ludashi ContentProfile enums lacked
# CONTENT_TYPE_PROTON and crashed on install with "type":"Proton". User
# confirmed current Winlator/Ludashi now accepts Proton type (2026-04-22),
# so both variants ship as Proton. Ludashi's WineInfo regex
#   ^(wine|proton)-([0-9.]+)-?([0-9.]+)?-(x86|x86_64|arm64ec)$
# matches `Proton-11.0-arm64ec-0` after strip-last-2 → `proton-11.0-arm64ec` →
# group1=proton, arch=arm64ec. Users see "Proton 11.0" label, correct arch.

mkdir -p "$(dirname "$OUTPUT_PREFIX")"

# Strip leading "proton-"/"wine-" so the Winlator verName doesn't double-prefix
# (Winlator synthesizes "Proton-<verName>-<verCode>"). Keep bare "11.0-arm64ec"
# so the regex matches cleanly and arch resolves to arm64ec.
WINE_VERSION_NAME="$VERSION_NAME"
WINE_VERSION_NAME="${WINE_VERSION_NAME#proton-}"
WINE_VERSION_NAME="${WINE_VERSION_NAME#wine-}"

# ---- A. Proton-type (.wcp) for WinNative ----
write_profile "$STAGING_DIR/profile.json" "Proton" "$VERSION_CODE"
(cd "$STAGING_DIR" && tar --owner=0 --group=0 --sort=name -cf "$TAR_TMP" *)
xz -T0 -9e --force -c "$TAR_TMP" > "${OUTPUT_PREFIX}.wcp"
echo "wrote ${OUTPUT_PREFIX}.wcp  ($(du -h "${OUTPUT_PREFIX}.wcp" | cut -f1))  [type=Proton verName=$VERSION_NAME]"

# ---- B. Proton-type (.wcp.xz) for Winlator / Ludashi / CMOD / Bionic ----
WINE_OUTPUT_PREFIX="$(dirname "$OUTPUT_PREFIX")/$(basename "$OUTPUT_PREFIX" | sed 's/^proton-/proton-wine-/')"
python3 - "$STAGING_DIR/profile.json" "$WINE_VERSION_NAME" "0" "$DESCRIPTION" "Proton" <<'PY'
import json, sys
out, ver, code, desc, ptype = sys.argv[1:6]
profile = {
    "type": ptype,
    "versionName": ver,
    "versionCode": int(code),
    "description": desc,
    "files": [],
    "wine": {"binPath": "bin", "libPath": "lib", "prefixPack": "prefixPack.txz"},
}
with open(out, "w") as f:
    json.dump(profile, f, indent=2)
print(f"wrote {out}")
PY
rm -f "$TAR_TMP"
(cd "$STAGING_DIR" && tar --owner=0 --group=0 --sort=name -cf "$TAR_TMP" *)
xz -T0 -9e --force -c "$TAR_TMP" > "${WINE_OUTPUT_PREFIX}.wcp.xz"
echo "wrote ${WINE_OUTPUT_PREFIX}.wcp.xz  ($(du -h "${WINE_OUTPUT_PREFIX}.wcp.xz" | cut -f1))  [type=Proton verName=$WINE_VERSION_NAME]"

( cd "$(dirname "$OUTPUT_PREFIX")" \
  && sha256sum "$(basename "$OUTPUT_PREFIX").wcp" \
               "$(basename "$WINE_OUTPUT_PREFIX").wcp.xz" ) \
  >> "$(dirname "$OUTPUT_PREFIX")/SHA256SUMS"
echo "done"
