# Maintainer: Beau-a <25479637+Beau-a@users.noreply.github.com>
pkgname=obs-pipewire-extended
pkgver=0.01
pkgrel=1
pkgdesc="OBS Studio plugin for per-application PipeWire audio stream isolation (RC)"
arch=('x86_64')
url="https://github.com/Beau-a/OBS-game-isolator"
license=('GPL-2.0-or-later')
depends=('obs-studio' 'pipewire' 'wireplumber')
makedepends=('cmake' 'pkgconf')
source=("$pkgname-$pkgver.tar.gz::$url/archive/refs/tags/v$pkgver.tar.gz")
sha256sums=('ad3f797732e8c27106b2e334a11d868a1290a21fe28eaf797b8b301239392b2f')

build() {
    cmake -B build -S "OBS-game-isolator-$pkgver" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr
    cmake --build build
}

package() {
    DESTDIR="$pkgdir" cmake --install build
}
