#!/bin/bash
# Builds the .deb, .rpm and Arch packages of the committed tree in clean
# containers, installs each one there and runs packaging/smoke-test.sh.
# Needs docker. Packages land in dist/.
#
#   packaging/build-packages.sh [deb] [rpm] [arch]     (default: all three)
set -u
cd "$(dirname "$0")/.."
ROOT=$PWD
VERSION=$(sed -n "s/^  version: '\(.*\)',/\1/p" meson.build)
WORK=$(mktemp -d)
OWNER=$(id -u):$(id -g)
TARGETS=${*:-deb rpm arch}
mkdir -p dist "$WORK/src" "$WORK/out"
git archive --prefix="videoshot-$VERSION/" HEAD | gzip > "$WORK/src/videoshot-$VERSION.tar.gz"

run() { # run <image> <script>
  docker run --rm -e OWNER="$OWNER" -e VERSION="$VERSION" \
    -v "$WORK/src:/src:ro" -v "$WORK/out:/out" -v "$ROOT/packaging:/packaging:ro" \
    "$1" bash -c "$2"
}

for target in $TARGETS; do
  echo "=== $target"
  case $target in
    deb) run ubuntu:24.04 '
      export DEBIAN_FRONTEND=noninteractive
      apt-get update -qq && apt-get install -y -qq --no-install-recommends build-essential devscripts equivs >/dev/null 2>&1
      mkdir /b && tar xzf /src/videoshot-$VERSION.tar.gz -C /b && cd /b/videoshot-$VERSION
      mk-build-deps -i -r -t "apt-get -y -qq --no-install-recommends" debian/control >/dev/null 2>&1
      dpkg-buildpackage -b -us -uc >/b/build.log 2>&1 || { tail -20 /b/build.log; exit 1; }
      apt-get install -y -qq --no-install-recommends /b/videoshot_*.deb xvfb dbus-daemon dbus-x11 >/dev/null 2>&1; echo "SMOKE install exit=$?"
      bash /packaging/smoke-test.sh
      cp /b/videoshot_*.deb /out/ && chown $OWNER /out/*' ;;
    rpm) run fedora:latest '
      dnf -y -q install rpm-build meson gcc gettext desktop-file-utils "pkgconfig(glib-2.0)" "pkgconfig(gtk+-3.0)" "pkgconfig(libhandy-1)" "pkgconfig(x11)" "pkgconfig(xext)" >/dev/null 2>&1
      mkdir -p /root/rpmbuild/SOURCES && cp /src/videoshot-$VERSION.tar.gz /root/rpmbuild/SOURCES/
      rpmbuild -bb /packaging/rpm/videoshot.spec >/root/build.log 2>&1 || { tail -20 /root/build.log; exit 1; }
      dnf -y -q install /root/rpmbuild/RPMS/*/videoshot-[0-9]*.rpm xorg-x11-server-Xvfb dbus-daemon >/dev/null 2>&1; echo "SMOKE install exit=$?"
      bash /packaging/smoke-test.sh
      cp /root/rpmbuild/RPMS/*/videoshot-[0-9]*.rpm /out/ && chown $OWNER /out/*' ;;
    arch) run archlinux:latest '
      pacman -Syu --noconfirm --needed base-devel meson gettext gtk3 libhandy libx11 libxext ffmpeg xorg-server-xvfb dbus >/dev/null 2>&1
      useradd -m b && mkdir /b && cp /packaging/arch/PKGBUILD /src/videoshot-$VERSION.tar.gz /b/ && chown -R b /b
      su b -c "cd /b && makepkg -f >/b/build.log 2>&1" || { tail -20 /b/build.log; exit 1; }
      pacman -U --noconfirm /b/videoshot-*.pkg.tar.zst >/dev/null 2>&1; echo "SMOKE install exit=$?"
      bash /packaging/smoke-test.sh
      cp /b/videoshot-*.pkg.tar.zst /out/ && chown $OWNER /out/*' ;;
    *) echo "unknown target: $target"; exit 2 ;;
  esac
done
cp "$WORK"/out/* dist/ 2>/dev/null
rm -rf "$WORK"
ls -l dist/
