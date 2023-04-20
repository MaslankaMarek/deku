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
	local mismatch=""
	[[ $localrelease != *"$kernelrelease"* ]] && mismatch=" release:$kernelrelease"
	[[ $localversion != *"$kernelversion"* ]] && mismatch+=" version:$kernelversion"
	[[ $mismatch == "" ]] && return
	logErr "Kernel image mismatch:$mismatch."
	logInfo "Kernel on the device: $kernelrelease $kernelversion"
	logInfo "Kernel on the host: $localrelease $localversion"
	return 1
}

main()
{
	if [ "$DEPLOY_TYPE" == "" ] || [ "$DEPLOY_PARAMS" == "" ]; then
		logWarn "Please setup connection parameters to target device"
		exit
	fi
	validateKernels

	bash $COMMANDS_DIR/build.sh
	local res=$?
	[ $res != 0 ] && exit 1

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
		[ ! -f "$moduledir/id" ] && { modulestounload+="-$module"; continue; }
		local localid=$(<$moduledir/id)
		[ "$id" == "$localid" ] && modulesontarget+=$module
	done <<< $(bash deploy/$DEPLOY_TYPE.sh --getids)

	local modules=`find $workdir -type d -name "deku_*" | tr '\n' ' '`
	read -a modules <<< "$modules"
	for moduledir in "${modules[@]}"; do
		[[ "$moduledir" == "" ]] && break
		local module=`basename $moduledir`
		[[ "${modulesontarget[*]}" =~ "${module}" ]] && continue;
		[[ "${modulestounload[*]}" =~ "${module}" ]] && continue;
		[[ -e "$moduledir/id" ]] && modulestoupload+="$moduledir/$module.ko "
	done

	if ((${#modulestoupload[@]} == 0)) && ((${#modulestounload[@]} == 0)); then
		echo "No modules need to upload"
		return
	fi

	modulestoupload=${modulestoupload[@]}
	modulestounload=${modulestounload[@]}
	bash "deploy/$DEPLOY_TYPE.sh" $modulestoupload $modulestounload
	res=$?
	[ $res != 0 ] && echo -e "${RED}Failed!${NC}"
	return $res
}

main $@
exit $?
