#!/bin/bash
# Build three Debian packages without root:
#   y720-cooling-dkms, y720-hid-dkms, y720-control (CLI + GUI).
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
version=$(cat VERSION)
arch=$(dpkg --print-architecture)
work="$root/build/deb"
dist="$root/dist"
rm -rf "$work" "$dist"
mkdir -p "$work" "$dist"

# Build CLI and GUI as the invoking user.
make -C userspace
meson setup "$work/gui-build" gui --prefix=/usr --buildtype=release
ninja -C "$work/gui-build"
DESTDIR="$work/control" meson install -C "$work/gui-build"
install -Dm755 userspace/build/y720-fan "$work/control/usr/bin/y720-fan"
install -Dm755 userspace/build/y720-backlight "$work/control/usr/bin/y720-backlight"

doc="$work/control/usr/share/doc/y720-control"
install -d "$doc"
for name in CLI.md USERSPACE-DRIVER-API.md GUI-INTEGRATION.md C-DEVELOPMENT.md FNF7.md COMPATIBILITY.md GAMEZONE.md; do
    install -m644 "docs/$name" "$doc/$name"
done
install -m644 README.md "$doc/README.md"
install -m644 LICENSE "$doc/copyright"

control="$work/control"
size=$(du -sk "$control" | awk '{print $1}')
mkdir -p "$control/DEBIAN"
cat > "$control/DEBIAN/control" <<CONTROL
Package: y720-control
Version: $version
Architecture: $arch
Maintainer: doublepama10 <doublepama10@users.noreply.github.com>
Section: utils
Priority: optional
Installed-Size: $size
Depends: libgtk-4-1 (>= 4.16), libadwaita-1-0, libjson-glib-1.0-0, libx11-6, pkexec | policykit-1
Recommends: y720-cooling-dkms, y720-hid-dkms
Homepage: https://github.com/doublepama10/y720_nerve_linux
Description: Y720 fan and keyboard backlight control (CLI and GTK4 GUI)
 Userspace control panel and command line tools for the Lenovo Y720-15IKB.
 The GUI provides fan status, temperature curves, keyboard backlight profiles,
 tray integration and configurable shortcuts. The same package contains
 y720-fan and y720-backlight.
CONTROL
cat > "$control/DEBIAN/postinst" <<'POSTINST'
#!/bin/sh
set -e
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database /usr/share/applications >/dev/null 2>&1 || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -q /usr/share/icons/hicolor >/dev/null 2>&1 || true
fi
exit 0
POSTINST
chmod 755 "$control/DEBIAN/postinst"
dpkg-deb --root-owner-group --build "$control" "$dist/y720-control_${version}_${arch}.deb"

build_dkms() {
    local package="$1" module="$2" component="$3"
    local stage="$work/$package"
    local srcdir="$stage/usr/src/$module-$version"
    mkdir -p "$srcdir/kernel" "$stage/DEBIAN" "$stage/usr/share/doc/$package"
    install -m644 "$component/dkms.conf" "$srcdir/dkms.conf"
    install -m644 "$component/kernel/Makefile" "$srcdir/kernel/Makefile"
    install -m644 LICENSES/GPL-2.0.txt "$srcdir/kernel/COPYING"
    for f in "$component"/kernel/*.c "$component"/kernel/*.h; do
        case "$f" in *.mod.c) continue ;; esac
        install -m644 "$f" "$srcdir/kernel/"
    done
    install -m644 LICENSE "$stage/usr/share/doc/$package/copyright"
    local size
    size=$(du -sk "$stage" | awk '{print $1}')
    cat > "$stage/DEBIAN/control" <<CONTROL
Package: $package
Version: $version
Architecture: all
Maintainer: doublepama10 <doublepama10@users.noreply.github.com>
Section: kernel
Priority: optional
Installed-Size: $size
Depends: dkms, make, gcc | clang
Homepage: https://github.com/doublepama10/y720_nerve_linux
Description: DKMS source for the Lenovo Y720 $module kernel driver
 Installs the $module DKMS source tree and attempts to build it for the
 running kernel. Kernel headers and a matching compiler are required.
CONTROL
    cat > "$stage/DEBIAN/postinst" <<POSTINST
#!/bin/sh
set -e
if [ "\$1" = configure ]; then
    if ! command -v dkms >/dev/null 2>&1; then
        echo "dkms is required; package installed source only" >&2
        exit 0
    fi
    dkms add -m $module -v $version || true
    if dkms build -m $module -v $version; then
        dkms install -m $module -v $version || true
    else
        echo "DKMS build for $module failed; install matching kernel headers and run dkms autoinstall" >&2
    fi
fi
exit 0
POSTINST
    cat > "$stage/DEBIAN/prerm" <<PRERM
#!/bin/sh
set -e
if [ "\$1" = remove ] && command -v dkms >/dev/null 2>&1; then
    dkms remove -m $module -v $version --all || true
fi
exit 0
PRERM
    cat > "$stage/DEBIAN/postrm" <<POSTRM
#!/bin/sh
set -e
if [ "\$1" = purge ]; then
    rm -rf /usr/src/$module-$version /var/lib/dkms/$module/$version
fi
exit 0
POSTRM
    chmod 755 "$stage/DEBIAN/postinst" "$stage/DEBIAN/prerm" "$stage/DEBIAN/postrm"
    dpkg-deb --root-owner-group --build "$stage" "$dist/${package}_${version}_all.deb"
}

build_dkms y720-cooling-dkms y720-cooling fan
build_dkms y720-hid-dkms y720-hid keyboard

echo "Built:"
ls -lh "$dist"
