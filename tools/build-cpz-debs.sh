#!/usr/bin/env bash
# Cross-builds two arm64 Debian packages for the CardputerZero (Pi CM0, Debian trixie arm64)
# on an x86-64 PC without root, Docker or qemu:
#
#   optic-cardputerzero_2.0.0_arm64.deb  - the CardputerZero UI (velvet, LVGL fbdev + evdev)
#   optic-de_2.0.0_arm64.deb             - Optic DE, the X11 desktop for the HDMI out
#
# Toolchain: Zig 0.13 (`zig cc` / `zig c++`, target aarch64-linux-gnu.2.38; libc++ linked
# statically into velvet, so no libstdc++ dependency). Sysroot: Debian trixie arm64 .debs fetched
# straight from deb.debian.org (checksums from the signed-by-Release Packages.xz) and unpacked
# with dpkg-deb -x. glibc headers/stubs come from Zig itself, so libc6-dev is NOT in the sysroot.
# fmt is built from Debian's fmtlib source tarball (deb.debian.org) and linked statically.
#
# Usage:   tools/build-cpz-debs.sh            (everything)
# Env:     WORK=<dir>        scratch dir (default ~/.cache/cpzdeb); debs land in $WORK/out
#          ZIG=<path>        zig 0.13 binary
#          OPTIC_DE_SRC=<d>  Optic DE sources (copied, never built in place)
#          LVGL_SRC=<d>      a local LVGL v9.5 checkout (optional; else fetched)
#          VELVET=<d>        the Velvet source tree (default: this script's parent)
#          JOBS=<n>
# Nothing here needs root; nothing is published or installed.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VELVET="${VELVET:-$(cd "$HERE/.." && pwd)}"
WORK="${WORK:-$HOME/.cache/cpzdeb}"
ZIG="${ZIG:-$(command -v zig || echo zig)}"
OPTIC_DE_SRC="${OPTIC_DE_SRC:-$HOME/build/opticsuite/runtime/src/main/assets/optic-de}"
LVGL_SRC="${LVGL_SRC:-}"   # a local LVGL v9.5 checkout; empty = CMake fetches it
JOBS="${JOBS:-$(nproc)}"
VERSION=2.0.0
MAINT="Zedward <273751795+zedward856-spec@users.noreply.github.com>"
TARGET=aarch64-linux-gnu.2.38
MA=aarch64-linux-gnu
DEB_MIRROR=https://deb.debian.org/debian
SUITE=trixie

DL="$WORK/dl"            # downloads only (indexes, .debs, source tarball)
SYSROOT="$WORK/sysroot"  # unpacked .debs
TC="$WORK/toolchain"     # our wrappers + CMake toolchain file
OUT="$WORK/out"
BUILD_V="$VELVET/build/arm64"
export ZIG_GLOBAL_CACHE_DIR="$WORK/zig-cache" ZIG_LOCAL_CACHE_DIR="$WORK/zig-cache"

log() { printf '\n==> %s\n' "$*"; }
mkdir -p "$DL/debs" "$DL/src" "$TC" "$OUT"

# ---------------------------------------------------------------------------------------------
# 1. Sysroot from trixie arm64 .debs
# ---------------------------------------------------------------------------------------------
ROOT_PKGS=(
  # velvet (device path: freetype, libpng, zlib; libjpeg only satisfies the CMake check, LVGL
  # decodes JPEG with its built-in TJPGD, so --as-needed drops it from NEEDED)
  libfreetype-dev libpng-dev zlib1g-dev libjpeg62-turbo-dev
  # Optic DE
  libx11-dev libxft-dev libxext-dev libxcursor-dev libxrender-dev libxcomposite-dev
  libxdamage-dev libxfixes-dev libxtst-dev libxi-dev libfontconfig-dev libdbus-1-dev
)
# never pulled into the sysroot: glibc/kernel headers + gcc runtime come from Zig, and
# tooling packages carry nothing to link against
EXCLUDE='^(libc6|libc6-dev|libc-dev-bin|libc-bin|linux-libc-dev|libgcc-s1|libstdc\+\+6|gcc-.*|cpp.*|libcrypt1|libcrypt-dev|rpcsvc-proto|libnsl.*|libtirpc.*|pkgconf.*|pkg-config|debconf.*|dpkg|perl.*|init-system-helpers|x11-common|xorg-sgml-doctools|libpython.*|python3.*|sensible-utils|tzdata|install-info|uuid-dev|libmd0|libbsd0|libbsd-dev|libmd-dev)$'

if [[ ! -s "$DL/Packages.xz" ]]; then
  log "fetch $SUITE arm64 Packages.xz + Sources.xz"
  curl -fsSL -o "$DL/Packages.xz" "$DEB_MIRROR/dists/$SUITE/main/binary-arm64/Packages.xz"
  curl -fsSL -o "$DL/Sources.xz" "$DEB_MIRROR/dists/$SUITE/main/source/Sources.xz"
fi

if [[ ! -f "$SYSROOT/.done" ]]; then
  log "resolve sysroot packages"
  # prints "<filename> <sha256>" for the root packages and their library dependencies
  python3 -I - "$DL/Packages.xz" "$EXCLUDE" "${ROOT_PKGS[@]}" > "$DL/sysroot.list" <<'PY'
import lzma, re, sys
idx, excl, roots = sys.argv[1], re.compile(sys.argv[2]), sys.argv[3:]
pk, provides, cur = {}, {}, {}
for line in lzma.open(idx, 'rt', encoding='utf-8', errors='replace').read().split('\n'):
    if not line.strip():
        if 'Package' in cur and cur['Package'] not in pk:
            pk[cur['Package']] = cur
            for p in cur.get('Provides', '').split(','):
                p = p.strip().split(' ')[0]
                if p: provides.setdefault(p, cur['Package'])
        cur = {}; continue
    if line[0] in ' \t': continue
    k, _, v = line.partition(':'); cur[k] = v.strip()
seen, todo = set(), list(roots)
while todo:
    n = todo.pop()
    if n not in pk: n = provides.get(n, n)
    if n in seen or excl.match(n): continue
    if n not in pk: sys.exit(f'missing package {n}')
    seen.add(n)
    for dep in (pk[n].get('Depends', '') + ',' + pk[n].get('Pre-Depends', '')).split(','):
        dep = dep.strip()
        if dep: todo.append(re.split(r'[ (:]', dep.split('|')[0].strip())[0])
for n in sorted(seen):
    print(pk[n]['Filename'], pk[n]['SHA256'], n, pk[n]['Version'])
PY
  log "download $(wc -l < "$DL/sysroot.list") .debs from $DEB_MIRROR"
  rm -rf "$SYSROOT"; mkdir -p "$SYSROOT"
  while read -r fn sha name ver; do
    f="$DL/debs/$(basename "$fn")"
    [[ -s "$f" ]] || curl -fsSL -o "$f" "$DEB_MIRROR/$fn"
    echo "$sha  $f" | sha256sum -c --quiet - || { echo "checksum mismatch: $f" >&2; exit 1; }
    dpkg-deb -x "$f" "$SYSROOT"
  done < "$DL/sysroot.list"
  # make absolute symlinks (e.g. lib*.so -> /usr/lib/...) relative to the sysroot
  find "$SYSROOT" -type l | while read -r l; do
    t="$(readlink "$l")"
    if [[ "$t" == /* ]]; then ln -sfn "$SYSROOT$t" "$l"; fi
  done
  touch "$SYSROOT/.done"
fi

# fmt: Debian's fmtlib source, built static by velvet's CMake (FetchContent source override)
FMT_TGZ="$DL/src/fmtlib_10.1.1+ds1.orig.tar.gz"
FMT_SHA=7000f214d2628f04174822f8048c573f1608194408d7931121595a03f1c18a17
if [[ ! -s "$FMT_TGZ" ]]; then
  log "fetch fmtlib source"
  curl -fsSL -o "$FMT_TGZ" "$DEB_MIRROR/pool/main/f/fmtlib/$(basename "$FMT_TGZ")"
fi
echo "$FMT_SHA  $FMT_TGZ" | sha256sum -c --quiet -
FMT_SRC="$WORK/fmt-src"
if [[ ! -f "$FMT_SRC/CMakeLists.txt" ]]; then
  rm -rf "$FMT_SRC"; mkdir -p "$FMT_SRC"
  tar -xzf "$FMT_TGZ" -C "$FMT_SRC" --strip-components=1
fi

# ---------------------------------------------------------------------------------------------
# 2. Zig wrappers, pkg-config setup, CMake toolchain file
# ---------------------------------------------------------------------------------------------
SYS_INC="-isystem $SYSROOT/usr/include/$MA -isystem $SYSROOT/usr/include"
SYS_LIB="-L$SYSROOT/usr/lib/$MA -L$SYSROOT/lib/$MA"
for t in cc c++; do
  cat > "$TC/zig-$t" <<EOF
#!/bin/sh
exec "$ZIG" $t -target $TARGET "\$@"
EOF
done
for t in ar ranlib; do
  printf '#!/bin/sh\nexec "%s" %s "$@"\n' "$ZIG" "$t" > "$TC/zig-$t"
done
chmod +x "$TC"/zig-*

cat > "$TC/aarch64-zig.cmake" <<EOF
# generated by build-cpz-debs.sh
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CM0_MULTIARCH $MA CACHE STRING "")
set(CM0_SDK_ROOT "$SYSROOT" CACHE PATH "")
set(CMAKE_C_COMPILER "$TC/zig-cc")
set(CMAKE_CXX_COMPILER "$TC/zig-c++")
set(CMAKE_AR "$TC/zig-ar" CACHE FILEPATH "")
set(CMAKE_RANLIB "$TC/zig-ranlib" CACHE FILEPATH "")
set(CMAKE_C_FLAGS_INIT "-isystem $SYSROOT/usr/include/$MA")
set(CMAKE_CXX_FLAGS_INIT "-isystem $SYSROOT/usr/include/$MA")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-Wl,--as-needed -Wl,--allow-shlib-undefined")
set(CMAKE_FIND_ROOT_PATH "$SYSROOT")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
EOF

export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/$MA/pkgconfig:$SYSROOT/usr/share/pkgconfig"
export PKG_CONFIG_PATH=

check_elf() {  # aarch64 + every NEEDED lib exists in the trixie sysroot (or is glibc)
  local f="$1" n
  file "$f" | grep -q 'ARM aarch64' || { echo "NOT aarch64: $f" >&2; return 1; }
  for n in $(readelf -d "$f" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p'); do
    case "$n" in libc.so.6|libm.so.6|libdl.so.2|libpthread.so.0|librt.so.1|ld-linux-aarch64.so.1) continue ;; esac
    [[ -e "$SYSROOT/usr/lib/$MA/$n" || -e "$SYSROOT/lib/$MA/$n" ]] || { echo "$f needs $n, not in sysroot" >&2; return 1; }
  done
  local g; g=$(readelf -V "$f" | grep -o 'GLIBC_2\.[0-9]*' | sort -Vu | tail -1)
  echo "  ok $(basename "$f"): $(readelf -d "$f" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' | tr '\n' ' ')(max ${g:-none})"
}

# Debian package(s) shipping the given sonames, looked up in the unpacked sysroot's file index
soname_pkgs() {
  local n p out=()
  if [[ ! -s "$DL/files.idx" ]]; then
    while read -r fn sha name ver; do
      dpkg-deb -c "$DL/debs/$(basename "$fn")" | awk -v p="$name" '{f=$6; sub(/^\./,"",f); print p, f}'
    done < "$DL/sysroot.list" > "$DL/files.idx"
  fi
  for n in "$@"; do
    case "$n" in libc.so.6|libm.so.6|libdl.so.2|libpthread.so.0|librt.so.1|ld-linux-aarch64.so.1) out+=(libc6); continue ;; esac
    p=$(awk -v n="$n" '$2 ~ ("/" n "$") {print $1; exit}' "$DL/files.idx")
    [[ -n "$p" ]] || { echo "no package ships $n" >&2; return 1; }
    out+=("$p")
  done
  printf '%s\n' "${out[@]}" | sort -u
}

# 100x100 launcher icon like the stock APPLaunch ones (dark rounded tile): the white Optic logo,
# with an optional small caption. Falls back to <fallback> without ImageMagick or the logo.
OPTIC_LOGO="${OPTIC_LOGO:-$HOME/Optic logo/optic-white/optic-transparent.png}"
make_icon() {  # <out.png> <caption or ""> <fallback.png>
  if command -v magick >/dev/null 2>&1 && [[ -f "$OPTIC_LOGO" ]]; then
    local sz=72 y=0
    [[ -n "$2" ]] && { sz=60; y=-10; }
    magick -size 100x100 xc:none -fill '#111318' -draw 'roundrectangle 0,0 99,99 22,22' \
      \( "$OPTIC_LOGO" -resize ${sz}x${sz} \) -gravity center -geometry +0+$y -composite \
      ${2:+-fill white -font DejaVu-Sans-Bold -pointsize 15 -gravity south -annotate +0+8 "$2"} \
      -strip -depth 8 "PNG32:$1"
  else
    cp "$3" "$1"
  fi
}

build_deb() {  # <stage> <outfile>
  find "$1" -type d -exec chmod 0755 {} +
  find "$1" -path "$1/DEBIAN" -prune -o -type f -perm /111 -exec chmod 0755 {} + \
       -o -type f -exec chmod 0644 {} +
  find "$1/DEBIAN" -type f ! -name 'post*' ! -name 'pre*' -exec chmod 0644 {} +
  dpkg-deb --root-owner-group -Zxz -b "$1" "$2" >/dev/null
  echo "built $2 ($(du -h "$2" | cut -f1))"
}

# ---------------------------------------------------------------------------------------------
# 3. Package 1: CardputerZero UI (velvet)
# ---------------------------------------------------------------------------------------------
V_PKG=optic-cardputerzero
V_APPDIR=/usr/share/APPLaunch/apps/$V_PKG      # AppBuilder's APP_INSTALL_DIR convention
log "velvet: configure $BUILD_V"
cmake -S "$VELVET" -B "$BUILD_V" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$TC/aarch64-zig.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DUSE_DESKTOP=OFF -DAPP_USE_DRM=OFF \
  -DCM0_SDK_ROOT="$SYSROOT" \
  -DCM0_ALLOW_FETCH_DEPS=ON \
  -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
  ${LVGL_SRC:+"-DFETCHCONTENT_SOURCE_DIR_LVGL=$LVGL_SRC"} \
  -DFETCHCONTENT_SOURCE_DIR_FMT="$FMT_SRC" \
  -DFETCHCONTENT_BASE_DIR="$BUILD_V/_deps" \
  -DAPP_NAME=velvet \
  -DAPP_ASSETS_ROOT="$V_APPDIR" \
  -DAPP_CONFIG_FILE=/etc/optic-cardputerzero.conf \
  -DAPP_DISPLAY_NAME=CardputerZero \
  -DAPP_INSTALL_SYSTEMD_SERVICE=OFF \
  -DPTHREAD_LIBRARY=pthread -DM_LIBRARY=m \
  -DBUILD_SHARED_LIBS=OFF -Wno-dev >"$WORK/velvet-configure.log"
log "velvet: build"
cmake --build "$BUILD_V" --target velvet -j "$JOBS" >"$WORK/velvet-build.log"

VS="$WORK/stage-velvet"; rm -rf "$VS"
mkdir -p "$VS/DEBIAN" "$VS/usr/share/APPLaunch/bin" "$VS/usr/share/APPLaunch/applications" \
         "$VS/usr/share/APPLaunch/share/images" "$VS$V_APPDIR" "$VS/etc" "$VS/usr/share/doc/$V_PKG"
"$ZIG" objcopy --strip-all "$BUILD_V/velvet" "$VS/usr/share/APPLaunch/bin/$V_PKG" 2>/dev/null \
  || cp "$BUILD_V/velvet" "$VS/usr/share/APPLaunch/bin/$V_PKG"
chmod 0755 "$VS/usr/share/APPLaunch/bin/$V_PKG"
for d in audio fonts images; do cp -r "$VELVET/assets/$d" "$VS$V_APPDIR/"; done
find "$VS$V_APPDIR" -name .DS_Store -delete
# the cp0-*.png device frames are only drawn by the desktop simulator; template*.png is unused
rm -f "$VS$V_APPDIR"/images/cp0-*.png "$VS$V_APPDIR"/images/template*.png
rmdir "$VS$V_APPDIR/images" 2>/dev/null || true
# launcher icon: in the shared APPLaunch images dir, so it gets a package-unique name
make_icon "$VS/usr/share/APPLaunch/share/images/$V_PKG.png" "" "$VELVET/assets/images/template_100.png"
install -m 644 "$VELVET/config/template-app.conf" "$VS/etc/optic-cardputerzero.conf"
install -m 644 "$VELVET/assets/fonts/LICENSE.txt" "$VS/usr/share/doc/$V_PKG/third-party-assets-license.txt"
[[ -f "$VELVET/LICENSE" ]] && install -m 644 "$VELVET/LICENSE" "$VS/usr/share/doc/$V_PKG/copyright"
cat > "$VS/usr/share/APPLaunch/applications/$V_PKG.desktop" <<EOF
[Desktop Entry]
Name=CardputerZero
Exec=/usr/share/APPLaunch/bin/$V_PKG
Terminal=false
Sysplause=true
Icon=share/images/$V_PKG.png
Type=Application
EOF
echo /etc/optic-cardputerzero.conf > "$VS/DEBIAN/conffiles"

log "velvet: verify ELF"
check_elf "$VS/usr/share/APPLaunch/bin/$V_PKG"
mapfile -t V_NEEDED < <(readelf -d "$VS/usr/share/APPLaunch/bin/$V_PKG" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p')
V_DEPS="$(soname_pkgs "${V_NEEDED[@]}" | paste -sd, | sed 's/,/, /g')"
cat > "$VS/DEBIAN/control" <<EOF
Package: $V_PKG
Version: $VERSION
Architecture: arm64
Maintainer: $MAINT
Section: APPLaunch
Priority: optional
Depends: $V_DEPS
Recommends: fonts-noto-cjk, libasound2t64 | libpulse0, mpv, yt-dlp, curl, ffmpeg, network-manager, tor, chromium
Description: CardputerZero - the Optic UI for the M5Stack CardputerZero
 Full-screen LVGL shell (internal name velvet) for the CardputerZero's
 320x170 screen and keypad: Linux framebuffer (/dev/fb0) + evdev input.
 Launched from APPLaunch. mpv, yt-dlp, curl, ffmpeg, NetworkManager, tor
 and chromium are optional runtime tools used by individual screens.
EOF
build_deb "$VS" "$OUT/${V_PKG}_${VERSION}_arm64.deb"

# ---------------------------------------------------------------------------------------------
# 4. Package 2: Optic DE
# ---------------------------------------------------------------------------------------------
DES="$WORK/optic-de-src"
log "optic-de: copy sources -> $DES"
rm -rf "$DES"; mkdir -p "$DES"
cp -a "$OPTIC_DE_SRC/." "$DES/"
( cd "$DES" && make clean >/dev/null )
# The Makefile calls $(CC) and pkg-config directly and has no LDFLAGS, so the sysroot search
# paths go into a CC wrapper (cc-sysroot); pkg-config resolves against the sysroot through the
# exported PKG_CONFIG_* variables. The Makefile probes the HOST's /usr/include/jpeglib.h for
# optic-view; the target's libjpeg is in the sysroot, so JPEG=yes takes the libjpeg branch.
# --as-needed keeps NEEDED to what each program really uses ($(LIBS) is shared by all apps).
cat > "$TC/cc-sysroot" <<EOF
#!/bin/sh
exec "$ZIG" cc -target $TARGET $SYS_INC "\$@" $SYS_LIB -Wl,--as-needed -Wl,--allow-shlib-undefined
EOF
chmod +x "$TC/cc-sysroot"
log "optic-de: build"
make -C "$DES" -j "$JOBS" all CC="$TC/cc-sysroot" JPEG=yes \
  >"$WORK/optic-de-build.log" 2>&1 || { tail -40 "$WORK/optic-de-build.log"; exit 1; }
grep -iE 'warning|error' "$WORK/optic-de-build.log" | head -20 || true

DS="$WORK/stage-de"; rm -rf "$DS"
P=/opt/optic-de
DE_BINS=(optic-de optic-settings optic-term optic-files optic-code optic-pwn optic-store optic-tasks
         optic-remote optic-notifyd optic-link optic-cpz optic-text optic-view optic-pdf optic-media
         optic-calc optic-cal)
DE_SCRIPTS=(optic-session optic-firefox optic-control)
DE_SHARE=(wallpaper.png logo.a8 bpreplay_bold.otf xterm.res gtk3.css gtk4.css gtk-settings.ini
          firefox-user.js firefox-userChrome.css cpz-skin.png cpz-skin-black.png xorg-tearfree.conf)
DE_DESKTOP=(optic-files optic-code optic-pwn optic-store optic-tasks optic-remote optic-cpz optic-text
            optic-view optic-pdf optic-media optic-calc optic-cal)
# the Makefile's /usr/local/bin links, put on /usr/bin instead (a package must not own /usr/local)
DE_LINKS=(optic-settings optic-term optic-files optic-code optic-pwn optic-store optic-tasks optic-remote
          optic-text optic-view optic-pdf optic-media optic-calc optic-cal optic-cpz optic-session
          optic-firefox optic-control optic-notifyd optic-link)

install -d "$DS/DEBIAN" "$DS$P/bin" "$DS$P/share/wallpapers" "$DS$P/lib" "$DS/usr/bin" \
           "$DS/usr/share/applications" "$DS/usr/share/xsessions" "$DS/usr/share/fonts/opentype/optic" \
           "$DS/usr/share/APPLaunch/applications" "$DS/usr/share/APPLaunch/share/images" \
           "$DS/usr/share/doc/optic-de"
for so in libsysvshm.so libcpzshim.so; do "$ZIG" objcopy --strip-debug "$DES/$so" "$DS$P/lib/$so"; chmod 755 "$DS$P/lib/$so"; done
for b in "${DE_BINS[@]}"; do "$ZIG" objcopy --strip-all "$DES/$b" "$DS$P/bin/$b" 2>/dev/null || cp "$DES/$b" "$DS$P/bin/$b"; chmod 755 "$DS$P/bin/$b"; done
for s in "${DE_SCRIPTS[@]}"; do install -m 755 "$DES/$s" "$DS$P/bin/"; done
for f in "${DE_SHARE[@]}"; do install -m 644 "$DES/$f" "$DS$P/share/"; done
if ls "$DES"/wallpapers/*.png >/dev/null 2>&1; then install -m 644 "$DES"/wallpapers/*.png "$DS$P/share/wallpapers/"; fi
install -m 644 "$DES/bpreplay_bold.otf" "$DS/usr/share/fonts/opentype/optic/"
for l in "${DE_LINKS[@]}"; do ln -s "$P/bin/$l" "$DS/usr/bin/$l"; done
for d in "${DE_DESKTOP[@]}"; do install -m 644 "$DES/$d.desktop" "$DS/usr/share/applications/"; done
install -m 644 "$DES/optic.desktop" "$DS/usr/share/xsessions/optic.desktop"
echo "$VERSION" > "$DS$P/share/VERSION"

# APPLaunch entry: start the desktop on the HDMI screen (UNTESTED on hardware)
cat > "$DS$P/bin/optic-de-hdmi" <<'EOF'
#!/bin/sh
# Optic DE on the CardputerZero's HDMI out, started from the APPLaunch launcher.
# UNTESTED ON REAL HARDWARE. What it does: starts a second X server (:1) on a free VT through
# xinit and runs optic-session in it; the CPZ's own 320x170 screen and APPLaunch stay as they are.
# The X server picks the HDMI output through KMS (vc4 / modesetting); with no HDMI monitor it may
# fail or open on the small panel's fbdev instead. Needs xserver-xorg-legacy (Xorg.wrap) so a
# non-root user may start X: /etc/X11/Xwrapper.config "allowed_users=anybody", "needs_root_rights=yes".
# Log: ~/.local/share/optic-de/hdmi.log . Stop it: log out from Optic DE's power menu, or
#   pkill -f 'optic-session'   (the X server ends with the session).
LOG="${XDG_DATA_HOME:-$HOME/.local/share}/optic-de"; mkdir -p "$LOG"; LOG="$LOG/hdmi.log"
DISP="${OPTIC_HDMI_DISPLAY:-:1}"
VT="${OPTIC_HDMI_VT:-vt2}"
if ! command -v xinit >/dev/null 2>&1; then echo "optic-de-hdmi: xinit missing (apt install xinit)" >"$LOG"; exit 1; fi
# detach from the launcher so APPLaunch gets its screen back right away
nohup setsid xinit /opt/optic-de/bin/optic-session -- /usr/bin/X "$DISP" "$VT" -nolisten tcp -keeptty \
  >"$LOG" 2>&1 </dev/null &
exit 0
EOF
chmod 755 "$DS$P/bin/optic-de-hdmi"
cat > "$DS/usr/share/doc/optic-de/README.Debian" <<'EOF'
optic-de for the CardputerZero / Raspberry Pi (arm64, Debian trixie)
====================================================================
Cross-built with Zig from the Optic Suite optic-de sources (build-cpz-debs.sh).
Layout = the Makefile's "make install", except that the command links live in
/usr/bin (not /usr/local/bin) and the BPreplay font in /usr/share/fonts/opentype/optic.

Start it:
 * from a display manager: pick the "Optic" X session (/usr/share/xsessions/optic.desktop)
 * from a console:         startx /opt/optic-de/bin/optic-session
 * from APPLaunch:         "Optic DE" -> /opt/optic-de/bin/optic-de-hdmi, which runs
                           xinit optic-session -- /usr/bin/X :1 vt2 in the background
                           (log: ~/.local/share/optic-de/hdmi.log)
The APPLaunch/HDMI path is UNTESTED on real hardware. A non-root X start needs
xserver-xorg-legacy; if X refuses, set in /etc/X11/Xwrapper.config:
    allowed_users=anybody
    needs_root_rights=yes
On a Raspberry Pi the postinst installs /etc/X11/xorg.conf.d/20-optic-tearfree.conf
(modesetting + TearFree), as make install does; it is removed with the package.
EOF
cat > "$DS/usr/share/APPLaunch/applications/optic-de.desktop" <<EOF
[Desktop Entry]
Name=Optic DE
Exec=$P/bin/optic-de-hdmi
Terminal=false
Sysplause=true
Icon=share/images/optic-de.png
Type=Application
EOF
make_icon "$DS/usr/share/APPLaunch/share/images/optic-de.png" "HDMI" "$DES/cpz-skin.png"

cat > "$DS/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if [ "$1" = configure ]; then
  command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database -q /usr/share/applications || true
  command -v fc-cache >/dev/null 2>&1 && fc-cache -f /usr/share/fonts/opentype/optic >/dev/null 2>&1 || true
  # the CardputerZero / any Raspberry Pi: tear-free HDMI (vc4 + modesetting), like make install
  if grep -qi 'raspberry pi' /proc/device-tree/model 2>/dev/null && [ ! -e /etc/X11/xorg.conf.d/20-optic-tearfree.conf ]; then
    mkdir -p /etc/X11/xorg.conf.d
    cp /opt/optic-de/share/xorg-tearfree.conf /etc/X11/xorg.conf.d/20-optic-tearfree.conf
  fi
fi
exit 0
EOF
cat > "$DS/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e
if [ "$1" = purge ] || [ "$1" = remove ]; then
  rm -f /etc/X11/xorg.conf.d/20-optic-tearfree.conf
  command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database -q /usr/share/applications || true
fi
exit 0
EOF
chmod 755 "$DS/DEBIAN/postinst" "$DS/DEBIAN/postrm"

log "optic-de: verify ELF"
NEEDED_ALL=()
for f in "$DS$P"/bin/* "$DS$P"/lib/*.so; do
  if file "$f" | grep -q ELF; then check_elf "$f"; mapfile -t -O "${#NEEDED_ALL[@]}" NEEDED_ALL < <(readelf -d "$f" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p'); fi
done
mapfile -t NEEDED_U < <(printf '%s\n' "${NEEDED_ALL[@]}" | sort -u)
DE_LIBDEPS="$(soname_pkgs "${NEEDED_U[@]}" | paste -sd, | sed 's/,/, /g')"
cat > "$DS/DEBIAN/control" <<EOF
Package: optic-de
Version: $VERSION
Architecture: arm64
Maintainer: $MAINT
Section: x11
Priority: optional
Depends: $DE_LIBDEPS, xserver-xorg-core, xserver-xorg-input-libinput, xserver-xorg-legacy, xinit, x11-xserver-utils, dbus-x11, fonts-dejavu-core
Recommends: xserver-xorg-video-fbdev, xterm, adwaita-icon-theme, pulseaudio, mpv, firefox-esr, ffmpeg, imagemagick, poppler-utils
Suggests: tigervnc-viewer, tigervnc-scraping-server, mupdf-tools, pwnagotchi
Description: Optic DE - X11 desktop for the CardputerZero HDMI out and Raspberry Pi
 The Optic desktop (window manager, panel, launcher, optional compositor)
 and its apps: settings, terminal, files, code and text editors, picture
 viewer, PDF reader, media player, calculator, calendar, task manager,
 remote desktop, add-on store, R36S link and the CardputerZero app player.
 Installs to /opt/optic-de (commands linked into /usr/bin), an "Optic" X
 session and an "Optic DE" APPLaunch entry that starts it on HDMI.
EOF
build_deb "$DS" "$OUT/optic-de_${VERSION}_arm64.deb"

log "done"
ls -l "$OUT"
