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

isKlpEnabled()
{
	local dir=$1

	grep -q "CONFIG_LIVEPATCH" "$dir/.config" || return $ERROR_KLP_IS_NOT_ENABLED
	grep -q "klp_enable_patch" "$dir/System.map" || return $ERROR_KLP_IS_NOT_ENABLED
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
	local deploytype="ssh"
	local deployparams=""
	local board=""
	local prebuild=""
	local postbuild=""
	local kernignorecrossrcinstall=""
	local ignorecros=""

	if ! options=$(getopt -u -o b:s:d:p:w: -l builddir:,sourcesdir:,deploytype:,\
				   deployparams:,src_inst_dir:,prebuild:,postbuild:,board:,workdir: \
				   target:,ssh_options:,ignore_cros: -- "$@")
	then
		exit 1
	fi

	while [ $# -gt 0 ]
	do
		local opt="$1"
		local value="$2"
		if [[ "$opt" =~ ^\-\-.+=.+ ]]; then
			value=${opt#*=}
			opt=${opt%%=*}
		else
			shift
		fi

		case $opt in
		-b|--builddir) builddir="$value" ;;
		-s|--sourcesdir) sourcesdir="$value" ;;
		-d|--deploytype) deploytype="$value" ;;
		-p|--deployparams) deployparams="$value" ;;
		-w|--workdir) workdir="$value" ;;
		--ssh_options) sshoptions="$value" ;;
		--board) board="$value" ;;
		--src_inst_dir) kernsrcinstall="$value" ;;
		--prebuild) prebuild="$value" ;;
		--postbuild) postbuild="$value" ;;
		--target) target="$value" ;;
		--ignore_cros) ignorecros="$value" ;;
		(--) shift; break;;
		(-*) logInfo "$0: Error - Unrecognized option $opt" 1>&2; exit 1;;
		(*) break;;
		esac
		shift
	done

	if [[ "$ignorecros" == "" && -e /etc/cros_chroot_version ]]; then
		if [[ "$board" == "" ]]; then
			logErr "Please specify the Chromebook board name using: $0 --board=<BOARD_NAME> ... syntax"
			exit $ERROR_NO_BOARD_PARAM
		fi

		if [[ ! -d "/build/$board" ]]; then
			logErr "Please setup the board using \"setup_board\" command"
			exit $ERROR_BOARD_NOT_EXISTS
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

		if [[ ! -f "$workdir/testing_rsa" ]]; then
			mkdir -p "$workdir"
			local GCLIENT_ROOT=~/chromiumos
			cp -f "${GCLIENT_ROOT}/src/third_party/chromiumos-overlay/chromeos-base/chromeos-ssh-testkeys/files/testing_rsa" "$workdir"
			chmod 0400 "$workdir/testing_rsa"
		fi

		if [[ "$sshoptions" == "" ]]; then
			sshoptions=" -o IdentityFile=$workdir/testing_rsa -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o BatchMode=yes -q"
		fi
		[[ ! " $target " =~ " @ " ]] && target="root@$target"
		deployparams="$target $sshoptions"
	elif [[ "$board" != "" ]]; then
		logInfo "--board parameter can be only used inside CrOS SDK"
		exit $ERROR_UNKNOWN
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

	logDebug "Initialize DEKU"
	logDebug "Sources dir: $sourcesdir"
	logDebug "Build dir: $builddir"
	logDebug "Work dir: $workdir"

	if [ -d "$workdir" ]
	then
		if [[ "$CHROMEOS_CHROOT" != 1 ]]; then
			[ "$(ls -A $workdir)" ] && { logErr "Directory \"$workdir\" is not empty"; exit $ERROR_WORKDIR_EXISTS; }
		fi
	else
		mkdir -p "$workdir" || { logErr "Failed to create directory \"$workdir\""; exit $?; }
	fi

	isKlpEnabled "$builddir"
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

	. ./header.sh

	local hash=
	[[ -f "$CONFIG_FILE" ]] && hash=`sed -rn "s/^WORKDIR_HASH=([a-f0-9]+)/\1/p" "$CONFIG_FILE"`
	[[ $hash == "" ]] && hash=$(generateDEKUHash)

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
	echo "WORKDIR_HASH=$hash" >> $CONFIG_FILE

	if [[ "$kernsrcinstall" == "" ]]; then
		git --work-tree="$sourcesdir" --git-dir="$workdir/.git" \
			-c init.defaultBranch=deku init > /dev/null
	fi

	mkdir -p "$SYMBOLS_DIR"
}

main "$@"
