# Maintainer: a-adz-1
pkgname=navidamp
pkgver=1.2.0
pkgrel=2
pkgdesc="Navidrome for Audacious: shuffle your library or playlists, with a Navidrome button and window for the Winamp Classic interface"
arch=('x86_64')
license=('GPL-3.0-only')
url="https://github.com/a-adz-1/navidamp"
depends=('audacious' 'audacious-plugins' 'qt6-base' 'glib2')
makedepends=('git' 'pkgconf')

# The Winamp interface part is a patched copy of the one in audacious-plugins,
# so it's built from that project's source at the matching release.
_audver=4.6.1
_audcommit=a7f3a1d1ad564b764137a3583cdff440d0230762  # tag audacious-plugins-4.6.1

source=('navidrome.cc' 'Makefile'
        'interface.mk' 'interface-config.h' 'navidrome-interface.patch'
        "audacious-plugins::git+https://github.com/audacious-media-player/audacious-plugins.git#commit=${_audcommit}")
sha256sums=('SKIP' 'SKIP' 'SKIP' 'SKIP' 'SKIP' 'SKIP')

prepare() {
  cd audacious-plugins
  # start from a clean copy, so this works when run more than once
  git checkout -q -- src/skins-qt
  git clean -qfd src/skins-qt
  patch -p1 < "$srcdir/navidrome-interface.patch"
}

build() {
  cd "$srcdir"
  make
  make -f interface.mk AP_SRC="$srcdir/audacious-plugins" CONFIG_H="$srcdir/interface-config.h"
}

package() {
  cd "$srcdir"
  make DESTDIR="$pkgdir" install
  make -f interface.mk AP_SRC="$srcdir/audacious-plugins" DESTDIR="$pkgdir" install
}
