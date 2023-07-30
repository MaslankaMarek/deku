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
	local originobj=$2
	local symvers=$3
	local ignoresymbols=(
						"_printk"
						)
	local syms=()
	local staticsymbols=(`readelf -W -s "$originobj" | \
						  awk 'BEGIN { ORS=" " } {
						  	if(($4 == "FUNC" || $4 == "OBJECT") && $5 == "LOCAL" ) {print $8}
						  }'`)
	local undsymbols=(`readelf -W -s "$module" | awk 'BEGIN { ORS=" " } { if($7 == "UND") {print $8} }'`)
	local greparg=`printf -- " -e \\s%s\\s" "${undsymbols[@]}"`
	local globalsymbols=`grep $greparg "$symvers" | tr '\n' ' '`
	read -a globalsymbols <<< "$globalsymbols"
	for sym in "${undsymbols[@]}"
	do
		[[ "$sym" == $DEKU_FUN_PREFIX* ]] && continue
		[[ " ${ignoresymbols[*]} " =~ " $sym " ]] && continue
		[[ " ${globalsymbols[*]} " =~ " $sym " ]] && continue
		echo "$sym"
	done
}

relocations()
{
	local moduledir=$1
	local module=$2
	local modsymfile="$moduledir/$MOD_SYMBOLS_FILE"
	local srcfile=$(<$moduledir/$FILE_SRC_PATH)
	local originobj="$BUILD_DIR/${srcfile%.*}.o"

	local syms=$(getSymbolsToRelocate "$moduledir/$module.ko" "$originobj" "$LINUX_HEADERS/Module.symvers")

	while read -r sym;
	do
		grep -q "\b$sym\b" "$modsymfile" && continue
		local objname=$(findObjWithSymbol "$sym" "$srcfile")
		if [[ $objname != "vmlinux" ]]; then
			local objpath=`find $SYMBOLS_DIR -type f -name "$objname"`
			objpath=${objpath#*$SYMBOLS_DIR/}.ko
			local cnt=`nm "$BUILD_DIR/$objpath" | grep "\b$sym\b" | wc -l`
			if [[ $cnt > 1 ]]; then
				logErr "A relocation is needed for the '$sym' function, which is located in the kernel module. This is not yet supported by DEKU."
			fi
		fi
		if [[ $(<$moduledir/$FILE_OBJECT) == "vmlinux" && $objname != "vmlinux" ]]; then
			logErr "The symbol '$sym' refers to the module '$objname' module. This is not yet supported by DEKU."
			exit $ERROR_UNSUPPORTED_REF_SYM_FROM_MODULE
		fi
		echo "$objname.$sym"
	done <<< "$syms"
}

findSymbolIndex()
{
	local -n index=$1
	local rel=$2
	local kofile=$3
	local objname=${rel%%.*}
	local symbol=${rel#*.}
	index=0
	[[ "$objname" != "vmlinux" ]] && return $NO_ERROR
	local mapfile="$SYSTEM_MAP"
	local count=`grep " $symbol$" "$mapfile" | wc -l`
	[[ $count == "1" ]] && return
	logInfo "Found $count occurrences of the symbol '$symbol'"
	local maches=`grep -A 10 -B 10 " $symbol$" "$mapfile" | cut -d " " -f 3`
	index=1
	local occure=0
	while read -r sym; do
		[[ "$sym" == "--" ]] && { occure=0; ((index++)); continue; }
		local found=`nm "$kofile" | grep "$sym"`
		if [[ "$found" != "" ]]; then
			((occure++))
			[[ $occure == "10" ]] && return $NO_ERROR
		fi
	done <<< "$maches"
	local filename=$(<`dirname $kofile`/$FILE_SRC_PATH)
	filename=`basename $filename`
	index=`readelf -a "$BUILD_DIR/vmlinux" | \
		grep -e "\b$filename\b" -e "\b$symbol$" | \
		grep -n $filename | \
		head -1 | \
		cut -f1 -d:`
	[[ "$index" != "" ]] && return $NO_ERROR
	logErr "Can't find index for symbol '$symbol'. This feature is not yet fully supported by DEKU"
	exit $ERROR_CANT_FIND_SYM_INDEX
}

main()
{
	local modules
	local prevmod=$(modulesList)
	bash generate_module.sh
	local rc=$?
	[[ $rc != 0 ]] && exit $rc

	modules=$(findModifiedModules "$prevmod")
	if [ -z "$modules" ]; then
		logInfo "No changes detected since last run"
		exit $NO_ERROR
	fi

	while read -r module; do
		local args=()
		local moduledir="$workdir/$module"
		local modsymfile="$moduledir/$MOD_SYMBOLS_FILE"
		local kofile="$moduledir/$module.ko"
		local objname=$(<$moduledir/$FILE_OBJECT)
		relocs=$(relocations "$moduledir" $module)
		local rc=${PIPESTATUS[0]}
		[[ $rc != 0 ]] && exit $rc

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
			if [[ $? != 0 ]]; then
				exit $ERROR_GENERATE_LIVEPATCH_MODULE
			fi
		else
			logDebug "Module does not need to adjust relocations"
		fi
	done <<< "$modules"
	logInfo "Generate DEKU module. Done"
}

main $@
