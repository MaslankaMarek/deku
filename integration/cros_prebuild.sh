#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (C) Semihalf, 2022
# Author: Marek Maślanka <mm@semihalf.com>

kerndir=`find /build/$CROS_BOARD/var/db/pkg/sys-kernel/ -type f -name "chromeos-kernel-*"`
kerndir=`basename $kerndir`
kerndir=${kerndir%-9999*}

afdo=`sed -nr 's/^(\w+\s)?AFDO_PROFILE_VERSION="(.*)"/\2/p' /build/$CROS_BOARD/var/db/pkg/sys-kernel/$kerndir-9999/$kerndir-9999.ebuild`
if [[ $afdo != "" ]]; then
	afdofile=$kerndir-$afdo.gcov
	afdopath=/var/cache/chromeos-cache/distfiles/$afdofile.xz
	[[ ! -f $afdopath ]] && afdopath=/build/$CROS_BOARD/tmp/portage/sys-kernel/$kerndir-9999/distdir/$afdofile.xz
	dstdir=/build/$CROS_BOARD/tmp/portage/sys-kernel/$kerndir-9999/work
	mkdir -p $dstdir
	if [[ -f $afdopath ]]; then
		cp -f $afdopath $dstdir/
		xz --decompress $dstdir/$afdofile.xz
		llvm-profdata merge \
			-sample \
			-extbinary \
			-output="$dstdir/$afdofile.extbinary.afdo" \
			"$dstdir/$afdofile"
		# Generate compbinary format for legacy compatibility
		llvm-profdata merge \
			-sample \
			-compbinary \
			-output="$dstdir/$afdofile.compbinary.afdo" \
			"$dstdir/$afdofile"
	else
		logWarn "Can't find afdo profile file ($afdopath)"
	fi
else
	logInfo "Can't find afdo profile file"
fi
