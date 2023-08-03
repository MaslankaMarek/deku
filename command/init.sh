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

checkKernelBuildDir()
{
	local dir=$1
	local workdir=$2

	local usellvm=
	isLLVMUsed "$dir" && usellvm=1

	local tmpdir=$(mktemp -d init-XXX --tmpdir="$workdir")
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
	out=`make -C $tmpdir LLVM=$usellvm 2>&1`
	local rc=$?
	rm -rf $tmpdir
	if [ $rc != 0 ]; then
		local kplerr=`echo "$out" | grep "klp_enable_patch"`
		if [ -n "$kplerr" ]; then
			return $ERROR_KLP_IS_NOT_ENABLED
		else
			logErr "$out"
			return $ERROR_UNKNOWN
		fi
	fi
	return $NO_ERROR
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
	local kernsrcinstall=""

	if ! options=$(getopt -u -o b:s:d:p:w: -l builddir:,sourcesdir:,deploytype:,deployparams:,srcinstdir:,prebuild:,postbuild:,board:,workdir: -- "$@")
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
		--srcinstdir) kernsrcinstall="$value" ;;
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
			exit $ERROR_NO_BOARD_PARAM
		fi
		local kerndir=`find /build/$board/var/db/pkg/sys-kernel/ -type f -name "chromeos-kernel-*"`
		kerndir=`basename $kerndir`
		kerndir=${kerndir%-9999*}
		[[ -z "$builddir" ]] && builddir="/build/$board/var/cache/portage/sys-kernel/$kerndir"
		prebuild="bash integration/cros_prebuild.sh"
		postbuild="bash integration/cros_postbuild.sh"
		if [[ "$kernsrcinstall" == "" ]]; then
			kernsrcinstall=`find /build/$board/usr/src/ -maxdepth 1 -type d \
								 -name "chromeos-kernel-*"`
		fi
		if [[ "$kernsrcinstall" == "" ]]; then
			logErr "Your kernel must be build with: USE=\"livepatch kernel_sources\" emerge-$board chromeos-kernel-..."
			exit $ERROR_INSUFFICIENT_BUILD_PARAMS
		fi
		[[ "$workdir" == "" ]] && workdir="workdir_$board"
		CONFIG_FILE="$workdir/config"
		deploytype="ssh"
	fi

	builddir=${builddir%/}
	workdir=${workdir%/}
	local linuxheaders="$builddir"

	if [[ -z "$builddir" ]]; then
		if [[ "$CHROMEOS_CHROOT" == 1 ]]; then
			logErr "Please specify the Chromebook board name using: $0 --board=<BOARD_NAME> ... syntax"
		else
			logErr "Please specify the kernel build directory using -b <PATH_TO_KERNEL_BUILD_DIR> syntax"
		fi
		exit 2
	fi

	[ -L "$builddir/source" ] && sourcesdir="$builddir/source"
	logDebug "Check for kernel sources in: $sourcesdir"
	isKernelSroucesDir $sourcesdir || sourcesdir="$builddir"

	sourcesdir=${sourcesdir%/}

	[ "$(git --version)" ] || { logErr "\"git\" could not be found. Please install \"git\""; exit 2; }

	logInfo "Initialize DEKU"
	logInfo "Sources dir: $sourcesdir"
	logInfo "Build dir: $builddir"
	logInfo "Work dir: $workdir"

	if [ -d "$workdir" ]
	then
		[ "$(ls -A $workdir)" ] && { logErr "Directory \"$workdir\" is not empty"; exit $ERROR_WORKDIR_EXISTS; }
	else
		mkdir -p "$workdir" || { logErr "Failed to create directory \"$workdir\""; exit $?; }
	fi

	checkKernelBuildDir "$builddir" "$workdir"
	local rc=$?
	if [[ $rc != $NO_ERROR ]]; then
		if [[ $rc == $ERROR_KLP_IS_NOT_ENABLED ]]; then
			if [[ "$CHROMEOS_CHROOT" == 1 ]]; then
				logErr "Your kernel must be build with: USE=\"livepatch kernel_sources\" emerge-$board chromeos-kernel-..."
				exit $ERROR_INSUFFICIENT_BUILD_PARAMS
			fi
			logErr "Kernel livepatching is not enabled. Please enable CONFIG_LIVEPATCH flag and rebuild the kernel"
			echo "Would you like to try enable this flag now? [y/n]"
			while true; do
				read -p "" yn
				case $yn in
					[Yy]* )
						enableKLP "$sourcesdir" && { logInfo "Flag is enabled. Please rebuild the kernel and try again."; exit $rc; } || logInfo "Failed do enable the flag. Please enable it manually."
						break;;
					[Nn]* ) exit $rc;;
					* ) echo "Please answer [y]es or [n]o.";;
				esac
			done
		else
			logErr "Given directory might not be a kernel build directory: \"$builddir\""
		fi
		exit $rc
	fi

	if ! isKernelSroucesDir $sourcesdir; then
		if [ "$sourcesdir" = "." ]; then
			logErr "Current directory is not a kernel srouces directory"
		else
			logErr "Given directory does not contains valid kernel sources: \"$sourcesdir\""
		fi
		exit $ERROR_INVALID_KERN_SRC_DIR
	fi

	[[ "$deploytype" == "" ]] && { logErr "Please specify deploy type -d [ssh]"; exit $ERROR_NO_DEPLOY_TYPE; }
	[[ "$deployparams" == "" ]] && { logErr "Please specify parameters for deploy \"$deploytype\". Use -p paramer"; exit $ERROR_NO_DEPLOY_PARAMS; }

	if [ ! -f "deploy/$deploytype.sh" ]; then
		logErr "Unknown deploy type '$deploytype'"
		exit $ERROR_INVALID_DEPLOY_TYPE
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
	[[ "$kernsrcinstall" != "" ]] && echo "KERN_SRC_INSTALL_DIR=\"$kernsrcinstall\"" >> $CONFIG_FILE
	isLLVMUsed "$linuxheaders" && echo "USE_LLVM=\"LLVM=1\"" >> $CONFIG_FILE
	echo "WORKDIR_HASH=$(generateDEKUHash)" >> $CONFIG_FILE

	if [[ "$kernsrcinstall" == "" ]]; then
		git --work-tree="$sourcesdir" --git-dir="$workdir/.git" \
			-c init.defaultBranch=deku init > /dev/null
	fi

	mkdir -p "$SYMBOLS_DIR"
}

main "$@"
