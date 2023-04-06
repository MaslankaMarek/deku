#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (C) Semihalf, 2022
# Author: Marek Maślanka <mm@semihalf.com>

isKernelSroucesDir()
{
	dir=$1
	if [ ! -f "$dir/Kbuild" ]; then
		return 1
	fi
	if [ ! -f "$dir/Kconfig" ]; then
		return 1
	fi
	if [ ! -f "$dir/Makefile" ]; then
		return 1
	fi
	return 0
}

isKernelBuildDir()
{
	local dir=$1

	local usellvm=
	isLLVMUsed "$dir" && usellvm=1

	local tmpdir=$(mktemp -d init-XXX --tmpdir=$workdir)
	cat > "$tmpdir/test.c" <<- EOF
	#include <linux/kernel.h>
	#include <linux/module.h>
	#include <linux/livepatch.h>
	static int deku_init(void)
	{
		return klp_enable_patch(NULL);
	}
	static void deku_exit(void)
	{
	}
	module_init(deku_init);
	module_exit(deku_exit);
	MODULE_INFO(livepatch, "Y");
	MODULE_LICENSE("GPL");
	EOF

	echo "obj-m += test.o" > "$tmpdir/Makefile"
	echo "all:" >> "$tmpdir/Makefile"
	echo "	make -C $1 M=\$(PWD)/$tmpdir modules" >> "$tmpdir/Makefile"
	res=`make -C $tmpdir LLVM=$usellvm 2>&1`
	rc=$?
	rm -rf $tmpdir
	if [ $rc -ne 0 ]; then
		local kplerr=`echo "$res" | grep "klp_enable_patch"`
		if [ -n "$kplerr" ]; then
			return 2
		else
			logDebug "$res"
			return 1
		fi
	fi
	return 0
}

isLLVMUsed()
{
	local linuxheaders=$1
	grep -qs "CONFIG_CC_IS_CLANG=y" "$linuxheaders/.config"
}

enableKLP()
{
	local sourcesdir=$1
	local configfile="$sourcesdir/chromeos/config/x86_64/common.config"
	[ ! -f "$configfile" ] && configfile="$sourcesdir/chromeos/config/chromeos/x86_64/common.config"
	[ ! -f "$configfile" ] && configfile="$builddir/.config"
	[ ! -f "$configfile" ] && return 1
	local flags=("CONFIG_KALLSYMS_ALL" "CONFIG_LIVEPATCH")
	for flag in "${flags[@]}"
	do
		bash $sourcesdir/scripts/config --file $configfile --enable $flag
	done
	grep -q "CONFIG_LIVEPATCH" "$configfile" && return 0
	return 1
}

main()
{
	local builddir=""
	local sourcesdir="."
	local deploytype=""
	local deployparams=""
	local board=""
	local prebuild=""
	local postbuild=""

	if ! options=$(getopt -u -o b:s:d:p:w: -l builddir:,sourcesdir:,deploytype:,deployparams:,prebuild:,postbuild:,board:,workdir: -- "$@")
	then
		exit 1
	fi

	while [ $# -gt 0 ]
	do
		local opt="$1"
		local value="$2"
		if [[ "$opt" =~ ^\-\-.+=.+ ]]; then
			value=${opt#*=}
			opt=${opt%=*}
		fi

		case $opt in
		-b|--builddir) builddir="$value" ; shift;;
		-s|--sourcesdir) sourcesdir="$value" ; shift;;
		-d|--deploytype) deploytype="$value" ; shift;;
		-p|--deployparams) deployparams="$value" ; shift;;
		-w|--workdir) workdir="$value" ; shift;;
		--board) board="$value" ;;
		--prebuild) prebuild="$value" ;;
		--postbuild) postbuild="$value" ;;
		(--) shift; break;;
		(-*) logInfo "$0: Error - Unrecognized option $opt" 1>&2; exit 1;;
		(*) break;;
		esac
		shift
	done

	if [[ "$board" ]]; then
		if [[ "$CHROMEOS_CHROOT" != 1 ]]; then
			logInfo "--board parameter can be only used inside CrOS SDK"
			exit 1
		fi
		local kerndir=`find /build/$board/var/db/pkg/sys-kernel/ -type f -name "chromeos-kernel-*"`
		kerndir=`basename $kerndir`
		kerndir=${kerndir%-9999*}
		[[ -z "$builddir" ]] && builddir="/build/$board/var/cache/portage/sys-kernel/$kerndir"
		prebuild="bash integration/cros_prebuild.sh"
		postbuild="bash integration/cros_postbuild.sh"
	fi

	builddir=${builddir%/}
	workdir=${workdir%/}
	local linuxheaders="$builddir"

	if [[ -z "$builddir" ]]; then
		if [[ "$CHROMEOS_CHROOT" != 1 ]]; then
			logErr "Please specify the kernel build directory using -b <PATH_TO_KERNEL_BUILD_DIR> syntax"
		else
			logErr "Please specify the ChromeOS board name using --board=<BOARD_NAME> syntax"
		fi
		exit 2
	fi

	[ -L "$builddir/source" ] && sourcesdir="$builddir/source"
	echo "Check for kernel sources in: $sourcesdir"
	isKernelSroucesDir $sourcesdir || sourcesdir="$builddir"

	sourcesdir=${sourcesdir%/}

	[ "$(git --version)" ] || { logErr "\"git\" could not be found. Please install \"git\""; exit 2; }

	echo "Initialize DEKU"
	echo "Sources dir: $sourcesdir"
	echo "Build dir: $builddir"
	echo "Work dir: $workdir"

	if [ -d "$workdir" ]
	then
		[ "$(ls -A $workdir)" ] && { logErr "Directory \"$workdir\" is not empty"; exit ENOTEMPTY; }
	else
		mkdir -p "$workdir"
	fi

	isKernelBuildDir $builddir
	local res=$?
	if [[ $res != 0 ]]; then
		if [[ $res == 2 ]]; then
			logErr "Kernel livepatching is not enabled. Please enable CONFIG_LIVEPATCH flag and rebuild the kernel"
			echo "Would you like to try enable this flag now? [y/n]"
			while true; do
				read -p "" yn
				case $yn in
					[Yy]* )
						enableKLP "$sourcesdir" && { logInfo "Flag was enabled. Pleas rebuild the kernel and try again."; exit 1; } || "Failed do enable the flag. Please enable it manually."
						break;;
					[Nn]* ) exit 2;;
					* ) echo "Please answer [y]es or [n]o.";;
				esac
			done
		else
			logErr "Given directory is not a kernel build directory: \"$builddir\""
		fi
		exit 2
	fi

	if ! isKernelSroucesDir $sourcesdir; then
		if [ "$sourcesdir" = "." ]; then
			logErr "Current directory is not a kernel srouces directory"
		else
			logErr "Given directory does not contains valid kernel sources: \"$sourcesdir\""
		fi
		exit 2
	fi

	[[ "$deploytype" == "" ]] && { logErr "Please specify deploy type -d [ssh]"; exit 2; }
	[[ "$deployparams" == "" ]] && { logErr "Please specify parameters for deploy \"$deploytype\". Use -p paramer"; exit 1; }

	if [ ! -f "deploy/$deploytype.sh" ]; then
		logErr "Unknown deploy type '$deploytype'"
		exit 2
	fi

	echo "BUILD_DIR=\"$builddir\"" > $CONFIG_FILE
	echo "SOURCE_DIR=\"$sourcesdir\"" >> $CONFIG_FILE
	echo "DEPLOY_TYPE=\"$deploytype\"" >> $CONFIG_FILE
	echo "DEPLOY_PARAMS=\"$deployparams\"" >> $CONFIG_FILE
	echo "MODULES_DIR=\"$builddir\"" >> $CONFIG_FILE
	echo "LINUX_HEADERS=\"$linuxheaders\"" >> $CONFIG_FILE
	echo "SYSTEM_MAP=\"$builddir/System.map\"" >> $CONFIG_FILE
	[[ "$prebuild" != "" ]] && echo "PRE_BUILD=\"$prebuild\"" >> $CONFIG_FILE
	[[ "$postbuild" != "" ]] && echo "POST_BUILD=\"$postbuild\"" >> $CONFIG_FILE
	[[ "$board" != "" ]] && echo "CROS_BOARD=\"$board\"" >> $CONFIG_FILE
	isLLVMUsed "$linuxheaders" && echo "USE_LLVM=\"LLVM=1\"" >> $CONFIG_FILE
	echo "WORKDIR_HASH=$(generateDEKUHash)" >> $CONFIG_FILE
	git --work-tree="$sourcesdir" --git-dir="$workdir/.git" init

	mkdir -p "$SYMBOLS_DIR"
}

main "$@"
