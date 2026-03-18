# Copyright 2024-2026 Gentoo Authors
# Distributed under the terms of the GNU General Public License v2

EAPI=8

DESCRIPTION="Attach and detach terminal sessions — transparent, no terminal emulation"
HOMEPAGE="https://github.com/mobydeck/atch"

if [[ ${PV} == 9999 ]]; then
	inherit git-r3
	EGIT_REPO_URI="https://github.com/mobydeck/atch.git"
else
	SRC_URI="https://github.com/mobydeck/atch/archive/${PV}.tar.gz -> ${P}.tar.gz"
	S="${WORKDIR}/${P}"
	KEYWORDS="~amd64 ~arm64"
fi

LICENSE="GPL-2"
SLOT="0"

DEPEND="sys-libs/glibc"
BDEPEND="app-text/pandoc"

src_compile() {
	emake STATIC_FLAG= VERSION="${PV}"
	emake man
}

src_install() {
	emake install DESTDIR="${D}" VERSION="${PV}"
	dodoc README.md
}
