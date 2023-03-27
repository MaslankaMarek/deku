#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (C) Semihalf, 2022
# Author: Marek Maślanka <mm@semihalf.com>
#
# Generate DEKU (livepatch) module from standard kernel module

modulesList()
{
	local results=()
	local modules=`find "$workdir" -maxdepth 1 -type d -regextype sed -regex ".\+/deku_[a-f0-9]\{8\}.\+" -printf "%f\n"`

	while read -r module; do
		local path="$workdir/$module/$module.ko"
		[ -f "$path" ] && results+=("$module `md5sum $path`")
	done <<< "$modules"
	printf "%s\n" "${results[@]}"
}

findModifiedModules()
{
	local results=()
	local prevmod=$1
	local newmod=$(modulesList)
	while read -r module; do
		[[ ! " ${prevmod[*]} " =~ " $module " ]] && results+=(${module%% *})
	done <<< "$newmod"
	printf "%s\n" "${results[@]}"
}

getSymbolsToRelocate()
{
	local module=$1
	local symvers=$2
	local syms=()
	local undsymbols=(`readelf -W -s "$module" | awk 'BEGIN { ORS=" " } { if($7 == "UND") {print $8} }'`)
	local greparg=`printf -- " -e \\s%s\\s" "${undsymbols[@]}"`
	local out=`grep $greparg "$symvers"`
	while read -r mod; do
		syms[${#syms[@]}]="$mod"
	done <<< "$out"
	for sym in "${undsymbols[@]}"
	do
		[[ "$sym" == $DEKU_FUN_PREFIX* ]] && continue
		[[ ! "${syms[*]}" =~ "$sym" ]] && echo "$sym"
	done
}

relocations()
{
	local moduledir=$1
	local module=$2
	local modsymfile="$moduledir/$MOD_SYMBOLS_FILE"

	local syms=$(getSymbolsToRelocate "$moduledir/$module.ko" "$LINUX_HEADERS/Module.symvers")
	while read -r sym;
	do
		grep -q "\b$sym\b" "$modsymfile" && continue
		local srcfile=$(<$moduledir/$FILE_SRC_PATH)
		local objname=$(findObjWithSymbol "$sym" "$srcfile")
		if [[ "$objname" == "" ]]; then
			logErr "Can't find symbol: $sym"
			exit 1
		fi
		echo "$objname.$sym"
	done <<< "$syms"
}

findSymbolIndex()
{
	local -n index=$1
	local rel=$2
	local kofile=$3
	local objname=${rel%.*}
	local symbol=${rel#*.}
	index=0
	[[ "$objname" != "vmlinux" ]] && return
	local mapfile="$SYSTEM_MAP"
	local count=`grep " $symbol$" "$mapfile" | wc -l`
	[[ $count == "1" ]] &&  return
	logInfo "Found $count occurrences of the symbol '$symbol'"
	local maches=`grep -A 5 -B 5 " $symbol$" "$mapfile" | cut -d " " -f 3`
	index=1
	local occure=0
	while read -r sym; do
		[[ "$sym" == "--" ]] && { occure=0; ((index++)); continue; }
		local found=`nm "$kofile" | grep "$sym"`
		if [[ "$found" != "" ]]; then
			((occure++))
			[[ $occure == "2" ]] && return
		fi
	done <<< "$maches"
	logErr "Can't find index for symbol '$symbol'"
	exit 1
}

main()
{
	local modules
	local prevmod=$(modulesList)
	bash generate_module.sh
	if [ $? -ne 0 ]; then
		logFatal "Failed"
		exit 2
	fi
	modules=$(findModifiedModules "$prevmod")
	if [ -z "$modules" ]; then
		logInfo "No changes detected since last run"
		exit
	fi

	while read -r module; do
		local args=()
		local moduledir="$workdir/$module"
		local modsymfile="$moduledir/$MOD_SYMBOLS_FILE"
		local kofile="$moduledir/$module.ko"
		local objname=$(<$moduledir/$FILE_OBJECT)
		local relocs=$(relocations "$moduledir" $module)
		logDebug "Processing $module..."
		if [[ "$relocs" != "" ]]; then
			while read -r sym; do
				args+=("-s $objname.$sym")
			done < $modsymfile

			while read -r rel; do
				local ndx=0
				findSymbolIndex ndx "$rel" "$kofile"
				args+=("-r $rel,$ndx")
				logDebug "Relocate \"$rel\""
			done <<< "$relocs"

			[[ "$LOG_LEVEL" > 0 ]] && args+=("-V")
			args+=("$kofile")
			logDebug "Make livepatch module"
			./mklivepatch ${args[@]}
			if [ $? -ne 0 ]; then
				logFatal "Abort!"
				exit 2
			fi
		else
			logDebug "Module does not need to adjust relocations"
		fi
	done <<< "$modules"
	logInfo "Generate DEKU module. Done"
}

main $@
exit $?
