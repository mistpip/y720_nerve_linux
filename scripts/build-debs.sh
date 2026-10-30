#!/bin/bash
# SPDX-License-Identifier: MIT
# Build three Debian packages without root:
#   y720-cooling-dkms, y720-hid-dkms, y720-control (CLI + GUI).
set -euo pipefail
umask 022
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
version=$(cat VERSION)
dpkg --validate-version "$version"
case "$version" in ''|*[!a-zA-Z0-9.+:~_-]*) echo 'Unsafe VERSION' >&2; exit 2 ;; esac
arch=$(dpkg --print-architecture)
mkdir -p "$root/build"
work=$(mktemp -d "$root/build/deb.XXXXXX")
dist=${Y720_DIST_DIR:-"$root/dist"}
mkdir -p "$work/artifacts" "$dist"

# Build CLI and GUI as the invoking user.
make -C userspace
meson setup "$work/gui-build" gui --prefix=/usr --buildtype=release
ninja -C "$work/gui-build"
DESTDIR="$work/debian/y720-control" meson install -C "$work/gui-build"
install -Dm755 userspace/build/y720-fan "$work/debian/y720-control/usr/bin/y720-fan"
install -Dm755 userspace/build/y720-backlight "$work/debian/y720-control/usr/bin/y720-backlight"

doc="$work/debian/y720-control/usr/share/doc/y720-control"
install -d "$doc/docs"
for name in CLI.md DEVELOPMENT.md; do
    install -m644 "docs/$name" "$doc/docs/$name"
done
install -m644 README.md "$doc/README.md"
install -m644 LICENSE "$doc/copyright"
install -m644 LICENSE "$doc/LICENSE"
install -d "$doc/LICENSES"
install -m644 LICENSES/*.txt "$doc/LICENSES/"

# Derive runtime dependencies from every installed ELF, not a handwritten subset.
mkdir -p "$work/debian" "$work/debian/y720-control/DEBIAN"
cat > "$work/debian/control" <<'CONTROL'
Source: y720-control
Section: utils
Priority: optional
Maintainer: mistpip <mistpip@users.noreply.github.com>

Package: y720-control
Architecture: any
Description: Y720 userspace control tools
CONTROL
shlibs=$(cd "$work" && dpkg-shlibdeps -O -e"$work/debian/y720-control/usr/bin/y720-control" \
    -e"$work/debian/y720-control/usr/bin/y720-fan" -e"$work/debian/y720-control/usr/bin/y720-backlight")
shlibs=${shlibs#shlibs:Depends=}

control="$work/debian/y720-control"
size=$(du -sk "$control" | awk '{print $1}')
mkdir -p "$control/DEBIAN"
cat > "$control/DEBIAN/control" <<CONTROL
Package: y720-control
Version: $version
Architecture: $arch
Maintainer: mistpip <mistpip@users.noreply.github.com>
Section: utils
Priority: optional
Installed-Size: $size
Depends: $shlibs, adwaita-icon-theme, sudo
Recommends: y720-cooling-dkms, y720-hid-dkms, xdg-desktop-portal
Homepage: https://github.com/mistpip/y720_nerve_linux
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
if command -v visudo >/dev/null 2>&1; then
    visudo -cf /etc/sudoers.d/y720-control
fi
POSTINST
cat "$root/scripts/refresh-device-access.sh" >> "$control/DEBIAN/postinst"
chmod 755 "$control/DEBIAN/postinst"
dpkg-deb --root-owner-group --build "$control" "$work/artifacts/y720-control_${version}_${arch}.deb"

build_dkms() {
    local package="$1" module="$2" component="$3"
    local stage="$work/$package"
    local srcdir="$stage/usr/src/$module-$version"
    mkdir -p "$srcdir/kernel" "$stage/DEBIAN" "$stage/usr/share/doc/$package"
    install -m644 "$component/dkms.conf" "$srcdir/dkms.conf"
    install -m644 VERSION "$srcdir/VERSION"
    install -m644 "$component/kernel/Makefile" "$srcdir/kernel/Makefile"
    install -m644 LICENSES/GPL-2.0.txt "$srcdir/kernel/COPYING"
    for f in "$component"/kernel/*.c "$component"/kernel/*.h; do
        case "$f" in *.mod.c) continue ;; esac
        install -m644 "$f" "$srcdir/kernel/"
    done
    install -m644 LICENSE "$stage/usr/share/doc/$package/copyright"
    install -d "$stage/usr/share/doc/$package/LICENSES"
    install -m644 LICENSES/*.txt "$stage/usr/share/doc/$package/LICENSES/"
    local size
    size=$(du -sk "$stage" | awk '{print $1}')
    cat > "$stage/DEBIAN/control" <<CONTROL
Package: $package
Version: $version
Architecture: all
Maintainer: mistpip <mistpip@users.noreply.github.com>
Section: kernel
Priority: optional
Installed-Size: $size
Depends: dkms, make, gcc | clang
Homepage: https://github.com/mistpip/y720_nerve_linux
Description: DKMS source for the Lenovo Y720 $module kernel driver
 Installs the $module DKMS source tree and builds it for the
 running kernel. Kernel headers and a matching compiler are required.
CONTROL
    cat > "$stage/DEBIAN/postinst" <<POSTINST
#!/bin/sh
set -e
if [ "\$1" = configure ]; then
    command -v dkms >/dev/null
    kernel=\$(uname -r)
    test -d "/lib/modules/\$kernel/build" || {
        echo "Matching headers for \$kernel required; install them and run dpkg --configure $package" >&2
        exit 1
    }
    if ! dkms status -m $module -v $version | grep -q .; then
        dkms add -m $module -v $version
    fi
    dkms build -m $module -v $version -k "\$kernel"
    dkms install -m $module -v $version -k "\$kernel"
fi
exit 0
POSTINST
    cat > "$stage/DEBIAN/prerm" <<PRERM
#!/bin/sh
set -e
case "\$1" in
    remove|upgrade)
        if [ -d /var/lib/dkms/$module/$version ]; then
            dkms remove -m $module -v $version --all
        fi ;;
esac
exit 0
PRERM
    cat > "$stage/DEBIAN/postrm" <<POSTRM
#!/bin/sh
set -e
if [ "\$1" = purge ] && [ -d /var/lib/dkms/$module/$version ]; then
    dkms remove -m $module -v $version --all
fi
exit 0
POSTRM
    chmod 755 "$stage/DEBIAN/postinst" "$stage/DEBIAN/prerm" "$stage/DEBIAN/postrm"
    dpkg-deb --root-owner-group --build "$stage" "$work/artifacts/${package}_${version}_all.deb"
}

build_dkms y720-cooling-dkms y720-cooling fan
build_dkms y720-hid-dkms y720-hid keyboard

echo "Built:"
for package in "$work/artifacts/"*.deb; do
    destination="$dist/${package##*/}"
    install -m644 "$package" "$destination.tmp.$$"
    mv -f -- "$destination.tmp.$$" "$destination"
    ls -lh -- "$destination"
done
echo "Staging retained for inspection: $work"
