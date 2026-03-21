#!/bin/sh
set -e

VERSION="${1:?usage: build-deb.sh VERSION}"
ARCH=$(dpkg --print-architecture)
PKGDIR="$(mktemp -d)"
trap 'rm -rf "$PKGDIR"' EXIT

SRCDIR="$(cd "$(dirname "$0")/../.." && pwd)"

make -C "$SRCDIR" clean
make -C "$SRCDIR" VERSION="$VERSION" STATIC_FLAG=
make -C "$SRCDIR" man
make -C "$SRCDIR" install DESTDIR="$PKGDIR" VERSION="$VERSION"

mkdir -p "$PKGDIR/DEBIAN"
cat > "$PKGDIR/DEBIAN/control" <<EOF
Package: atch
Version: ${VERSION}
Section: utils
Priority: optional
Architecture: ${ARCH}
Depends: libc6
Maintainer: mobydeck <noreply@github.com>
Homepage: https://github.com/mobydeck/atch
Description: Attach and detach terminal sessions
 Transparent terminal session manager with persistent
 session history and multi-client attach support.
EOF

dpkg-deb --build "$PKGDIR" "${SRCDIR}/atch_${VERSION}_${ARCH}.deb"
echo "Built: atch_${VERSION}_${ARCH}.deb"
