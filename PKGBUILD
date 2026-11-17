# Maintainer: Øystein Lie <oeystein@gengartv.com>
# Contributor: Chris Sawczuk (ChrisAJS) - USB protocol reverse-engineering & lgx2userspace

pkgname=avermedia-gc550-dkms
_pkgname=gc550
pkgver=0.1.3
pkgrel=1
pkgdesc="AVerMedia Live Gamer Extreme (GC550) V4L2 + ALSA USB 3.0 Capture Kernel Driver (DKMS)"
arch=('x86_64' 'aarch64')
url="https://github.com/GengarTV/LGXgc550-KernelModule"
license=('GPL-2.0-only OR MIT')
depends=('dkms')
provides=('gc550-dkms' 'avermedia-gc550')
conflicts=('gc550-dkms' 'avermedia-gc550')

package() {
	local src_root="${startdir}"
	local dkms_dest="${pkgdir}/usr/src/${_pkgname}-${pkgver}"

	# Strict 12-file DKMS source manifest (matching install.sh allowlist)
	local manifest_root=(
		"Makefile"
		"Kbuild"
		"dkms.conf"
	)
	local manifest_kernel=(
		"gc550-audio.c"
		"gc550-cmd.c"
		"gc550-core.c"
		"gc550-diag.c"
		"gc550-diag.h"
		"gc550.h"
		"gc550-i2c.c"
		"gc550-regtables.h"
		"gc550-video.c"
	)

	install -dm755 "${dkms_dest}/kernel"
	for f in "${manifest_root[@]}"; do
		install -Dm644 "${src_root}/${f}" "${dkms_dest}/${f}"
	done
	for f in "${manifest_kernel[@]}"; do
		install -Dm644 "${src_root}/kernel/${f}" "${dkms_dest}/kernel/${f}"
	done

	# udev rule
	install -Dm644 "${src_root}/99-avermedia-gc550.rules" \
		"${pkgdir}/usr/lib/udev/rules.d/99-avermedia-gc550.rules"

	# License files
	install -Dm644 "${src_root}/LICENSE-MIT" \
		"${pkgdir}/usr/share/licenses/${pkgname}/LICENSE-MIT"
	install -Dm644 "${src_root}/LICENCE.md" \
		"${pkgdir}/usr/share/licenses/${pkgname}/LICENCE.md"
	install -Dm644 "${src_root}/LICENSE-GPL-2.0" \
		"${pkgdir}/usr/share/licenses/${pkgname}/LICENSE-GPL-2.0"
	install -Dm644 "${src_root}/LICENSES.md" \
		"${pkgdir}/usr/share/licenses/${pkgname}/LICENSES.md"
}
