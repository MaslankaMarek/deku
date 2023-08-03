#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (C) Semihalf, 2023
# Author: Marek Maślanka <mm@semihalf.com>

. ./header.sh

main()
{
	export CHROMEOS_CHROOT=1

	local board=
	local isinit=
	local workdir=
	for ((i=1; i<=$#; i++))
	do
		opt=${!i}
		if [[ $opt == "--board="* ]]; then
			board="${!i#*=}"
		fi
		if [[ $opt == "--board" ]]; then
			((i++))
			board="${!i}"
		elif [[ $opt == "-w" ]]; then
			((i++))
			workdir="${!i}"
		elif [[ $opt == init ]]; then
			isinit=1
		fi
	done

	if [[ "$board" == "" ]]; then
		logErr "Please specify the Chromebook board name using: $0 --board=<BOARD_NAME> ... syntax"
		exit $ERROR_NO_BOARD_PARAM
	fi

	if [[ ! -d "/build/$board" ]]; then
		logErr "Please setup the board using \"setup_board\" command"
		exit $ERROR_BOARD_NOT_EXISTS
	fi

	[[ "$workdir" == "" ]] && workdir="workdir_$board"
	export CROS_WORKDIR="$workdir"

	if [[ $isinit ]]; then
		if [ -e "$workdir/config" ]; then
			logErr "DEKU is already initialzed for the $board board"
			exit $ERROR_WORKDIR_EXISTS
		fi
		return $NO_ERROR
	fi

	if [[ ! -d "$workdir" ]]; then
		logErr "Please initialize project for the $board board using following command:"
		logErr "$0 --board=$board -p root@<DUT_ADDRESS[:PORT]> init"
		exit $ERROR_NO_WORKDIR
	fi

	local kernsrcinstall=`find /build/$board/usr/src/ -maxdepth 1 -type d \
							   -name "chromeos-kernel-*"`

	if [[ "$kernsrcinstall" == "" ]]; then
		logErr "Your kernel must be build with: USE=\"livepatch kernel_sources\" emerge-$board chromeos-kernel-..."
		exit $ERROR_INSUFFICIENT_BUILD_PARAMS
	fi

	local kerndir=`basename $kernsrcinstall`
	kerndir=${kerndir%-9999*}
	local builddir="/build/$board/var/cache/portage/sys-kernel/$kerndir"

	if [[ ! -d "$builddir" ]]; then
		logErr "Can't find build dir for the $board board"
		logErr "Please build the kernel with: USE=\"livepatch kernel_sources\" emerge-$board chromeos-kernel-..."
		exit $ERROR_INSUFFICIENT_BUILD_PARAMS
	fi

	sed -i s#^BUILD_DIR=.*#BUILD_DIR="$builddir"#g "$workdir/config"
	sed -i s#^SOURCE_DIR=.*#SOURCE_DIR="$builddir/source"#g "$workdir/config"
	sed -i s#^MODULES_DIR=.*#MODULES_DIR="$builddir"#g "$workdir/config"
	sed -i s#^LINUX_HEADERS=.*#LINUX_HEADERS="$builddir"#g "$workdir/config"
	sed -i s#^SYSTEM_MAP=.*#SYSTEM_MAP="$builddir/System.map"#g "$workdir/config"
	sed -i s#^KERN_SRC_INSTALL_DIR=.*#KERN_SRC_INSTALL_DIR="$kernsrcinstall"#g "$workdir/config"
}

main "$@"
