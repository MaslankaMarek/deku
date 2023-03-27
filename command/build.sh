#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (C) Semihalf, 2022
# Author: Marek Maślanka <mm@semihalf.com>

main()
{
	local syncversion=$(<"$KERNEL_VERSION_FILE")
	local localversion=$(getKernelVersion)
	if [[ "$syncversion" != "$localversion" ]]; then
		logWarn "Kernel image in build directory has changed from last run. You must undo any changes made after kernel was rebuild and run 'make sync'."
		exit 2
	fi

	# remove old modules from workdir
	local validmodules=()
	for file in $(modifiedFiles)
	do
		validmodules+=$(generateModuleName "$file")
	done
	while read moduledir
	do
		[[ $moduledir == "" ]] && break
		local module=`basename $moduledir`
		[[ ! " ${validmodules[*]} " =~ "$module" ]] && rm -rf  "$moduledir"
	done <<< "`find $workdir -type d -name deku_*`"

	echo "Build DEKU module"

	bash generate_hotreload.sh
	local res=$?
	[ $res != 0 ] && exit 1
	return 0
}

main $@
exit $?
