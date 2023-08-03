#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (C) Semihalf, 2022
# Author: Marek Maślanka <mm@semihalf.com>

validateKernels()
{
	local kernelrelease=`bash deploy/$DEPLOY_TYPE.sh --kernel-release`
	local kernelversion=`bash deploy/$DEPLOY_TYPE.sh --kernel-version`
	local localrelease=$(getKernelReleaseVersion)
	local localversion=$(getKernelVersion)
	[[ $localrelease == *"$kernelrelease"* && \
	   $localversion == *"$kernelversion"* ]] && return $NO_ERROR
	logErr "The kernel on the device is outdated!"
	logInfo "Kernel on the device: $kernelrelease $kernelversion"
	logInfo "Kernel on the host:   $localrelease $localversion"
	return $ERROR_INVALID_KERNEL_ON_DEVICE
}

main()
{
	if [ "$DEPLOY_TYPE" == "" ] || [ "$DEPLOY_PARAMS" == "" ]; then
		logWarn "Please set the connection parameters to the target device"
		exit $ERROR_NO_DEPLOY_PARAMS
	fi
	validateKernels
	local rc=$?
	if [[ "$KERN_SRC_INSTALL_DIR" && $rc != $NO_ERROR ]]; then
		logWarn "Please install the current built kernel on the device"
		exit $rc
	fi

	bash $COMMANDS_DIR/build.sh
	rc=$?
	[ $rc != $NO_ERROR ] && exit $rc

	# find modules need to upload and unload
	local modulestoupload=()
	local modulesontarget=()
	local modulestounload=()
	while read -r line
	do
		[[ "$line" == "" ]] && break
		local module=${line% *}
		local id=${line##* }
		local moduledir="$workdir/$module/"
		[ ! -f "$moduledir/id" ] && { modulestounload+=(-$module); continue; }
		local localid=$(<$moduledir/id)
		[ "$id" == "$localid" ] && modulesontarget+=($module)
	done <<< $(bash deploy/$DEPLOY_TYPE.sh --getids)

	local modules=`find $workdir -type d -name "deku_*" | tr '\n' ' '`
	read -a modules <<< "$modules"
	for moduledir in "${modules[@]}"; do
		[[ "$moduledir" == "" ]] && break
		local module=`basename $moduledir`
		[[ "${modulesontarget[*]}" =~ "${module}" ]] && continue;
		[[ "${modulestounload[*]}" =~ "${module}" ]] && continue;
		[[ -e "$moduledir/id" ]] && modulestoupload+=("$moduledir/$module.ko")
	done

	if ((${#modulestoupload[@]} == 0)) && ((${#modulestounload[@]} == 0)); then
		logDebug "No modules need to upload"
		return $NO_ERROR
	fi

	modulestoupload=${modulestoupload[@]}
	modulestounload=${modulestounload[@]}
	bash "deploy/$DEPLOY_TYPE.sh" $modulestoupload $modulestounload
	return $?
}

main $@
