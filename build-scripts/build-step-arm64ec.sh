#!/bin/bash

export ARCH="aarch64"
export WIN_ARCH="arm64ec,aarch64,i386"
export OUTPUT_DIR="/home/max/compiled-files-aarch64-esync-perf-lto"

export deps="$HOME/termuxfs/aarch64/data/data/com.termux/files/usr"
export RUNTIME_PATH="/data/data/com.winnative.cmod/files/imagefs/usr"
export install_dir=/data/data/com.winnative.cmod/files/imagefs/opt/proton-11-arm64ec-esync

#export TOOLCHAIN="$HOME/Android/android-ndk-r27d/toolchains/llvm/prebuilt/linux-x86_64/bin"
export TOOLCHAIN="$HOME/Android/Sdk/ndk/27.3.13750724/toolchains/llvm/prebuilt/linux-x86_64/bin"
export LLVM_MINGW_TOOLCHAIN="$HOME/toolchains/llvm-mingw-20250920-ucrt-ubuntu-22.04-x86_64/bin"

# Optional 16 KB page-size build for Android 15+ devices that ship with
# 16 KB-page kernels. When --enable-16kb-pages is passed, the API level is
# raised so the resulting .so files carry the alignment metadata Bionic's
# loader requires on those kernels, and the linker is told to align ELF
# segments to 16 KB. Default builds remain on API 28 with 4 KB alignment.
ENABLE_16KB_PAGES=0
for _arg in "$@"; do
  case "$_arg" in
    --enable-16kb-pages) ENABLE_16KB_PAGES=1 ;;
  esac
done
if [ "$ENABLE_16KB_PAGES" = "1" ]; then
  export TARGET=aarch64-linux-android35
  PAGE_SIZE_CFLAGS="-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES"
  PAGE_SIZE_LDFLAGS="-Wl,-z,max-page-size=16384"
  echo "16 KB page-size support: enabled (target $TARGET)"
else
  export TARGET=aarch64-linux-android28
  PAGE_SIZE_CFLAGS=""
  PAGE_SIZE_LDFLAGS=""
fi
export PATH=$LLVM_MINGW_TOOLCHAIN:$PATH

if [ "${USE_CCACHE:-0}" = "1" ] && command -v ccache >/dev/null 2>&1; then
  CCACHE_PREFIX="ccache "
  # Wine's PE-side Makefile rules invoke bare `clang`/`clang++` for the
  # arm64ec/aarch64/i386 windows targets, resolved via PATH. Insert wrapper
  # scripts that route those calls through ccache + llvm-mingw's clang so
  # the cross-compile (the bulk of the build) is also cached.
  CCWRAP="$HOME/.ccache-wrap-arm64ec"
  mkdir -p "$CCWRAP"
  cat > "$CCWRAP/clang" <<EOF
#!/bin/sh
exec ccache "$LLVM_MINGW_TOOLCHAIN/clang" "\$@"
EOF
  cat > "$CCWRAP/clang++" <<EOF
#!/bin/sh
exec ccache "$LLVM_MINGW_TOOLCHAIN/clang++" "\$@"
EOF
  chmod +x "$CCWRAP/clang" "$CCWRAP/clang++"
  export PATH="$CCWRAP:$PATH"
else
  CCACHE_PREFIX=""
fi
export CC="${CCACHE_PREFIX}$TOOLCHAIN/$TARGET-clang"
export AS="$CC"
export CXX="${CCACHE_PREFIX}$TOOLCHAIN/$TARGET-clang++"
export AR=$TOOLCHAIN/llvm-ar
export LD=$TOOLCHAIN/ld
export RANLIB=$TOOLCHAIN/llvm-ranlib
export STRIP=$TOOLCHAIN/llvm-strip
export DLLTOOL=$LLVM_MINGW_TOOLCHAIN/llvm-dlltool

export PKG_CONFIG_LIBDIR=$deps/lib/pkgconfig:$deps/share/pkgconfig
export ACLOCAL_PATH=$deps/lib/aclocal:$deps/share/aclocal
export CPPFLAGS="-idirafter $deps/include --sysroot=$TOOLCHAIN/../sysroot"

export C_OPTS="-Wno-declaration-after-statement -Wno-implicit-function-declaration -Wno-int-conversion -ffunction-sections -fdata-sections"
# WinNative perf+LTO build: -O3, ARMv8 baseline features (+lse for modern
# atomics, +crc for DXVK/vkd3d hashing, +crypto for TLS/AES), plus ThinLTO
# on the ELF/unix side only. PE (arm64ec/aarch64/i386) side uses its own
# CROSSCFLAGS pipeline via llvm-mingw and is NOT touched by these vars; if
# you want PE LTO too, patch the Makefile's CROSSCFLAGS separately.
export PERF_OPTS="-O3 -march=armv8-a+crypto+crc+lse -fomit-frame-pointer -fno-semantic-interposition"
export LTO_OPTS="-flto=thin"
export CFLAGS="$C_OPTS $PERF_OPTS $LTO_OPTS $PAGE_SIZE_CFLAGS --sysroot=$TOOLCHAIN/../sysroot"
export CXXFLAGS="$C_OPTS $PERF_OPTS $LTO_OPTS $PAGE_SIZE_CFLAGS --sysroot=$TOOLCHAIN/../sysroot"
# LDFLAGS: pass -flto=thin so the driver runs the LTO codegen stage at
# link time; -Wl,-plugin-opt=mcpu preserves the ARM extensions through
# post-LTO codegen so the linker doesn't regress to generic armv8-a.
# Bake a placeholder rpath so the linker emits a DT_RUNPATH section we can
# rewrite after install with patchelf to a $ORIGIN-relative path. Trying to
# pass a literal $ORIGIN through autoconf's LDFLAGS into make's recipes
# loses to make's $-expansion ($O is a make variable reference); patchelf
# avoids the escaping fight entirely. See --install step below.
export LDFLAGS="-L$deps/lib -Wl,-rpath=$RUNTIME_PATH/lib $LTO_OPTS -Wl,-plugin-opt=mcpu=armv8-a+crypto+crc+lse -Wl,--gc-sections $PAGE_SIZE_LDFLAGS"

export FREETYPE_CFLAGS="-I$deps/include/freetype2"
export PULSE_CFLAGS="-I$deps/include/pulse"
export PULSE_LIBS="-L$deps/lib/pulseaudio -lpulse"
export SDL2_CFLAGS="-I$deps/include/SDL2"
export SDL2_LIBS="-L$deps/lib -lSDL2"
export X_CFLAGS="-I$deps/include/X11"
export X_LIBS="-landroid-sysvshm"
export GSTREAMER_CFLAGS="-I$deps/include/gstreamer-1.0 -I$deps/include/glib-2.0 -I$deps/lib/glib-2.0/include -I$deps/glib-2.0/include -I$deps/lib/gstreamer-1.0/include"
export GSTREAMER_LIBS="-L$deps/lib -lgstgl-1.0 -lgstapp-1.0 -lgstvideo-1.0 -lgstaudio-1.0 -lglib-2.0 -lgobject-2.0 -lgio-2.0 -lgsttag-1.0 -lgstbase-1.0 -lgstreamer-1.0"
export FFMPEG_CFLAGS="-I$deps/include/libavutil -I$deps/include/libavcodec -I$deps/include/libavformat"
export FFMPEG_LIBS="-L$deps/lib -lavutil -lavcodec -lavformat"

for arg in "$@"
do
  if [ "$arg" == "--build-sysvshm" ];
  then
    # Build android_sysvshm library
    SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

    if [ -d "$PROJECT_ROOT/android/android_sysvshm" ]; then
        echo "Building android_sysvshm library..."
        cd "$PROJECT_ROOT/android/android_sysvshm"
        # Propagate the parent build's NDK API level + page-size flags so the
        # resulting .so matches the rest of the package. Without this the
        # sysvshm .so would always be API 28 / 4 KB-aligned, even when the
        # main build is API 35 / 16 KB-aligned, and the loader would refuse
        # to dlopen it on 16 KB-page kernels.
        SYSVSHM_TARGET="$TARGET" \
        SYSVSHM_CFLAGS="$PAGE_SIZE_CFLAGS" \
        SYSVSHM_LDFLAGS="$PAGE_SIZE_LDFLAGS" \
        ./build-aarch64.sh
        if [ $? -eq 0 ]; then
            echo "android_sysvshm built successfully"
            # Copy the library to deps/lib for linking
            mkdir -p "$deps/lib"
            cp build-aarch64/libandroid-sysvshm.so "$deps/lib/"
            echo "Copied libandroid-sysvshm.so to $deps/lib/"
        else
            echo "Warning: android_sysvshm build failed"
        fi
        cd "$PROJECT_ROOT"
    fi
  fi

  if [ "$arg" == "--configure" ];
  then
    ./configure \
      --enable-archs=$WIN_ARCH \
      --host=$TARGET \
      --prefix $install_dir \
      --bindir $install_dir/bin \
      --libdir $install_dir/lib \
      --exec-prefix $install_dir \
      --with-mingw=clang \
      --with-wine-tools=./wine-tools \
      --enable-win64 \
      --disable-win16 \
      --enable-nls \
      --disable-amd_ags_x64 \
      --enable-wineandroid_drv=no \
      --disable-tests \
      --with-alsa \
      --without-capi \
      --without-coreaudio \
      --without-cups \
      --without-dbus \
      --without-ffmpeg \
      --without-fontconfig \
      --with-freetype \
      --without-gcrypt \
      --without-gettext \
      --with-gettextpo=no \
      --without-gphoto \
      --with-gnutls \
      --without-gssapi \
      --with-gstreamer \
      --without-inotify \
      --without-krb5 \
      --without-netapi \
      --without-opencl \
      --with-opengl \
      --without-osmesa \
      --without-oss \
      --without-pcap \
      --without-pcsclite \
      --without-piper \
      --with-pthread \
      --with-pulse \
      --without-sane \
      --with-sdl \
      --without-udev \
      --without-unwind \
      --without-usb \
      --without-v4l2 \
      --without-vosk \
      --with-vulkan \
      --without-wayland \
      --without-xcomposite \
      --without-xcursor \
      --without-xfixes \
      --without-xinerama \
      --without-xrandr \
      --without-xrender \
      --without-xshape \
      --with-xshm \
      --without-xxf86vm

    echo "Applying patches..."

    PATCHES=(
      # android network patch
      "android_network.patch"
      # (already in bleeding-edge) # "dlls_nsiproxy_sys_ip_c.patch"

      # midi support
      "midi_support.patch"

      # sdl patch
      "dlls_winebus_sys_bus_sdl_c.patch"

      # shm_utils
      "dlls_ntdll_unix_esync_c.patch"
      # (GE-style pre-exclude) # "dlls_ntdll_unix_fsync_c.patch"
      "server_esync_c.patch"
      # (GE-style pre-exclude) # "server_fsync_c.patch"

      # winex11
      # (already in bleeding-edge) # "dlls_winex11_drv_x11drv_h.patch"
      "dlls_winex11_drv_bitblt_c.patch"
      "dlls_winex11_drv_desktop_c.patch"
      # (already in bleeding-edge) # "dlls_winex11_drv_mouse_c.patch"
      # (already in bleeding-edge) # "dlls_winex11_drv_window_c.patch"
      "dlls_winex11_drv_keyboard_c.patch"
      # (already in bleeding-edge) # "dlls_winex11_drv_x11drv_main_c.patch"

      # address space patches
      # (already in bleeding-edge) # "dlls_ntdll_unix_virtual_c.patch"
      "loader_preloader_c.patch"

      # syscall Patches
      "dlls_ntdll_unix_signal_x86_64_c.patch"

      # pulse Patches
      "dlls_winepulse_drv_pulse_c.patch"

      # desktop patches
      "programs_explorer_desktop_c.patch"

      # path patches
      "dlls_ntdll_unix_server_c.patch"

      # winlator patches
      "dlls_amd_ags_x64_unixlib_c.patch"
      # (already in bleeding-edge) # "dlls_winex11_drv_opengl_c.patch"

      # shortcut patch
      # (already in bleeding-edge) # "programs_winemenubuilder_winemenubuilder_c.patch"

      # advapi32 patches
      "dlls_advapi32_advapi_c.patch"

      # browser patches
      "programs_winebrowser_makefile_in.patch"
      "programs_winebrowser_main_c.patch"

      # clipboard patches
      "dlls_user32_makefile_in.patch"
      "dlls_user32_clipboard_c.patch"
      "dlls_win32u_clipboard_c.patch"

      # fexcore patch
      # (already in bleeding-edge) # "dlls_ntdll_loader_c.patch"
      # (already in bleeding-edge) # "dlls_ntdll_unix_loader_c.patch"
      # (already in bleeding-edge) # "dlls_wow64_syscall_c.patch"
      # (already in bleeding-edge) # "loader_wine_inf_in.patch"

      # fix build
      "programs_wineboot_wineboot_c.patch"
      # (Wine 11 no-op) # "dlls_wdscore_wdscore_spec.patch"
      # 1. Extended State (XSTATE/YMM) Support Patches
      # (already in bleeding-edge) # "test-bylaws/dlls_ntdll_unwind_h.patch"
      # (already in bleeding-edge) # "test-bylaws/include_winnt_h.patch"

      # 2. Thread Suspension Patches
      # (GE-style pre-exclude) # "test-bylaws/dlls_ntdll_signal_arm64_c.patch"
      # (GE-style pre-exclude) # "test-bylaws/dlls_ntdll_signal_arm64ec_c.patch"
      # (GE-style pre-exclude) # "test-bylaws/dlls_ntdll_signal_x86_64_c.patch"
      # (already in bleeding-edge) # "test-bylaws/dlls_ntdll_ntdll_spec.patch"
      # (already in bleeding-edge) # "test-bylaws/dlls_ntdll_ntdll_misc_h.patch"
      # (already in bleeding-edge) # "test-bylaws/dlls_wow64_process_c.patch"
      # (already in bleeding-edge) # "test-bylaws/dlls_wow64_wow64_spec.patch"

      # 3. Process and Virtual Memory Management
      # (already in bleeding-edge) # "test-bylaws/dlls_wow64_virtual_c.patch"
      # (already in bleeding-edge) # "test-bylaws/server_process_c.patch"
      # (already in bleeding-edge) # "test-bylaws/dlls_ntdll_unix_process_c.patch"

      # 4. Server and Threading Infrastructure
      # (already in bleeding-edge) # "test-bylaws/server_thread_h.patch"
      # (already in bleeding-edge) # "test-bylaws/server_thread_c.patch"
      # (already in bleeding-edge) # "test-bylaws/dlls_ntdll_unix_thread_c.patch"

      # 5. Internal Headers
      # (already in bleeding-edge) # "test-bylaws/include_winternl_h.patch"

      # 6. Build System (Optional)
#      # (already in bleeding-edge) # "test-bylaws/tools_makedep_c.patch"
        "shell32_shlfileop_init_path_components.patch"
        "explorer_startmenu_shutdown_latch.patch"
    )

    for patch in "${PATCHES[@]}"; do
#      if git apply --check ./android/patches/$patch 2>/dev/null; then
        if [[ "$patch" == test-bylaws/* ]]; then _patch_marker=""; case "$patch" in   test-bylaws/dlls_ntdll_signal_arm64ec_c.patch) _patch_marker="ARM64EC_NT_XCONTEXT" _patch_file="dlls/ntdll/signal_arm64ec.c" ;;   test-bylaws/dlls_ntdll_signal_arm64_c.patch)   _patch_marker="RtlWow64SuspendThread" _patch_file="dlls/ntdll/signal_arm64.c" ;;   test-bylaws/dlls_ntdll_signal_x86_64_c.patch)  _patch_marker="RtlWow64SuspendThread" _patch_file="dlls/ntdll/signal_x86_64.c" ;;   test-bylaws/dlls_ntdll_ntdll_spec.patch)        _patch_marker="RtlWow64SuspendThread" _patch_file="dlls/ntdll/ntdll.spec" ;;   test-bylaws/dlls_ntdll_ntdll_misc_h.patch)      _patch_marker="pWow64SuspendLocalThread" _patch_file="dlls/ntdll/ntdll_misc.h" ;;   test-bylaws/tools_makedep_c.patch)              _patch_marker="arch_install_dirs[arch] = strmake( \"$(libdir)/wine/%s-windows\", archs.str[arch] );" _patch_file="tools/makedep.c" ;;   test-bylaws/server_thread_h.patch)              _patch_marker="bypass_proc_suspend" _patch_file="server/thread.h" ;;   test-bylaws/server_thread_c.patch)              _patch_marker="bypass_proc_suspend" _patch_file="server/thread.c" ;;   test-bylaws/server_process_c.patch)             _patch_marker="bypass_proc_suspend" _patch_file="server/process.c" ;;   test-bylaws/dlls_ntdll_unix_thread_c.patch)     _patch_marker="BYPASS_PROCESS_FREEZE" _patch_file="dlls/ntdll/unix/thread.c" ;;   test-bylaws/include_winternl_h.patch)           _patch_marker="ProcessFexHardwareTso" _patch_file="include/winternl.h" ;; esac; if [[ -n "$_patch_marker" ]] && grep -qF "$_patch_marker" "$_patch_file" 2>/dev/null; then   echo "ALREADY APPLIED (skipped): $patch"; elif git apply --ignore-whitespace -C1 --check ./android/patches/$patch 2>/dev/null && git apply --ignore-whitespace -C1 ./android/patches/$patch; then   echo "Applied: $patch"; elif git apply --3way --ignore-space-change ./android/patches/$patch 2>/dev/null; then   echo "Applied (3way): $patch"; elif patch -p1 --forward --batch --ignore-whitespace -i ./android/patches/$patch 2>/dev/null; then   echo "Applied (patch): $patch"; elif git apply --ignore-whitespace -C1 -R --check ./android/patches/$patch 2>/dev/null; then   echo "ALREADY APPLIED (skipped): $patch"; else   echo "ERROR: critical patch failed: $patch"; exit 1; fi; else if [[ "$patch" == "dlls_user32_makefile_in.patch" ]] && grep -qF "DELAYIMPORTS = imm32 combase ws2_32" "dlls/user32/Makefile.in" 2>/dev/null; then   echo "ALREADY APPLIED (skipped): $patch"; elif [[ "$patch" == "dlls_winex11_drv_x11drv_h.patch" ]] && grep -qF "XATOM__NET_WM_HWND" "dlls/winex11.drv/x11drv.h" 2>/dev/null; then   echo "ALREADY APPLIED (skipped): $patch"; elif [[ "$patch" == "dlls_winex11_drv_x11drv_main_c.patch" ]] && grep -qF ""_NET_WM_HWND"" "dlls/winex11.drv/x11drv_main.c" 2>/dev/null && grep -qF "#ifdef HAVE_X11_EXTENSIONS_XINPUT2_H" "dlls/winex11.drv/x11drv_main.c" 2>/dev/null; then   echo "ALREADY APPLIED (skipped): $patch"; elif [[ "$patch" == "dlls_winex11_drv_opengl_c.patch" ]] && grep -qF "int wine_x11forceglx = 0;" "dlls/winex11.drv/opengl.c" 2>/dev/null && grep -qF "atoi(getenv("WINE_X11FORCEGLX"))" "dlls/winex11.drv/opengl.c" 2>/dev/null && grep -qF "|| wine_x11forceglx)" "dlls/winex11.drv/opengl.c" 2>/dev/null; then   echo "ALREADY APPLIED (skipped): $patch"; elif [[ "$patch" == "programs_winemenubuilder_winemenubuilder_c.patch" ]] && grep -qF "icons\\hicolor" "programs/winemenubuilder/winemenubuilder.c" 2>/dev/null && grep -qF "_wgetenv( L"WINECONFIGDIR" )" "programs/winemenubuilder/winemenubuilder.c" 2>/dev/null && grep -qF "fprintf(file, "wine %s", escape(path));" "programs/winemenubuilder/winemenubuilder.c" 2>/dev/null; then   echo "ALREADY APPLIED (skipped): $patch"; elif git apply --ignore-whitespace -C1 -R --check ./android/patches/$patch 2>/dev/null; then   echo "ALREADY APPLIED (skipped): $patch"; elif git apply --ignore-whitespace -C1 --check ./android/patches/$patch 2>/dev/null && git apply --ignore-whitespace -C1 ./android/patches/$patch; then   echo "Applied: $patch"; elif git apply --3way --ignore-space-change ./android/patches/$patch 2>/dev/null; then   echo "Applied (3way): $patch"; elif patch -p1 --forward --batch --ignore-whitespace -i ./android/patches/$patch 2>/dev/null; then   echo "Applied (patch): $patch"; else   echo "ERROR: required patch failed: $patch"; exit 1; fi; fi
#      fi
    done
    if [ -x ./tools/make_requests ]; then ./tools/make_requests; fi
    if [ -x ./config.status ]; then ./config.status; fi
  fi

  if [ "$arg" == "--build" ]
  then
    echo "Building..."
    rm -rf $OUTPUT_DIR/bin
    rm -rf $OUTPUT_DIR/lib
    rm -rf $OUTPUT_DIR/share
    rm -rf $install_dir
    make -j$(nproc)
  fi

  if [ "$arg" == "--install" ]
  then
    echo "Installing..."
    mkdir -p $OUTPUT_DIR/bin
    mkdir -p $OUTPUT_DIR/lib
    mkdir -p $OUTPUT_DIR/share
    mkdir -p $install_dir
    make install -j$(nproc)
    cp -r $install_dir/bin/wine* $OUTPUT_DIR/bin
    cp -r $install_dir/bin/reg* $OUTPUT_DIR/bin
    cp -r $install_dir/bin/msi* $OUTPUT_DIR/bin
    cp -r $install_dir/bin/notepad $OUTPUT_DIR/bin
    cp -r $install_dir/lib/wine  $OUTPUT_DIR/lib
    cp -r $install_dir/share/wine  $OUTPUT_DIR/share

    # Rewrite the baked-in absolute rpath with a $ORIGIN-relative one so
    # the package finds <APP>/files/imagefs/usr/lib regardless of which
    # container app installs it. Two entries cover both ELF host layouts:
    #   bin/wineserver          → 4 ups to <APP>/files
    #   lib/wine/<arch>-unix/.so → 6 ups to <APP>/files
    if command -v patchelf >/dev/null 2>&1; then
      RPATH_REL='$ORIGIN/../../../../imagefs/usr/lib:$ORIGIN/../../../../../../imagefs/usr/lib'
      for f in "$OUTPUT_DIR/bin/wineserver" "$OUTPUT_DIR/bin/wine-preloader"; do
        [ -f "$f" ] && patchelf --set-rpath "$RPATH_REL" "$f" 2>/dev/null
      done
      find "$OUTPUT_DIR/lib/wine" -type f -name '*.so' -exec patchelf --set-rpath "$RPATH_REL" {} \; 2>/dev/null
      find "$OUTPUT_DIR/lib/wine" -type f -name 'wine' -exec patchelf --set-rpath "$RPATH_REL" {} \; 2>/dev/null
      find "$OUTPUT_DIR/lib/wine" -type f -name 'wine-preloader' -exec patchelf --set-rpath "$RPATH_REL" {} \; 2>/dev/null
      echo "patchelf: rewrote rpath to $RPATH_REL"
    else
      echo "patchelf not found; rpath stays baked-in (com.winnative.cmod path)"
    fi
  fi
done
