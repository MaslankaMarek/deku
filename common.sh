#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (C) Semihalf, 2022
# Author: Marek Maślanka <mm@semihalf.com>
#
# Common functions

logDebug()
{
	[[ "$LOG_LEVEL" > 0 ]] && return
	echo "[DEBUG] $1"
}
export -f logDebug

logInfo()
{
	[[ "$LOG_LEVEL" > 1 ]] && return
	echo "$1"
}
export -f logInfo

logWarn()
{
	[[ "$LOG_LEVEL" > 2 ]] && return
	echo -e "$ORANGE$1$NC"
}
export -f logWarn

logErr()
{
	echo -e "$1" >&2
}
export -f logErr

logFatal()
{
	echo -e "$RED$1$NC" >&2
}
export -f logFatal

filenameNoExt()
{
	[[ $# = 0 ]] && set -- "$(cat -)" "${@:2}"
	local basename=`basename "$1"`
	echo ${basename%.*}
}
export -f filenameNoExt

generateSymbols()
{
	local kofile=$1
	local path=`dirname $kofile`
	path=${path#*$MODULES_DIR}
	local outfile="$SYMBOLS_DIR/$path/"
	mkdir -p "$outfile"
	outfile+=$(filenameNoExt "$kofile")
	nm -f posix "$kofile" | cut -d ' ' -f 1,2 > "$outfile"
}
export -f generateSymbols

findObjWithSymbol()
{
	local sym=$1
	local srcfile=$2

	local out=`grep -lr "\b$sym\b" $SYMBOLS_DIR`
	[ "$out" != "" ] && { echo $(filenameNoExt "$out"); return; }

	local srcpath=$SOURCE_DIR/
	local modulespath=$MODULES_DIR/
	srcpath+=`dirname $srcfile`
	modulespath+=`dirname $srcfile`
	while true; do
		local files=`find "$modulespath" -maxdepth 1 -type f -name "*.ko"`
		if [ "$files" != "" ]; then
			while read -r file; do
				symfile=$(filenameNoExt "$file")
				[ -f "$SYMBOLS_DIR/$symfile" ] && continue
				generateSymbols $file
			done <<< "$files"

			out=`grep -lr "\b$sym\b" $SYMBOLS_DIR`
			[ "$out" != "" ] && { echo $(filenameNoExt "$out"); return; }
		fi
		[ -f "$srcpath/Kconfig" ] && break
		srcpath+="/.."
		modulespath+="/.."
	done
	grep -q "\b$sym\b" "$SYSTEM_MAP" && { echo vmlinux; return; }

	# not found
}
export -f findObjWithSymbol

getKernelVersion()
{
	sed -n "s/.*UTS_VERSION\ \"\(.\+\)\"$/\1/p" "$LINUX_HEADERS/include/generated/compile.h"
}
export -f getKernelVersion

getKernelReleaseVersion()
{
	sed -n "s/.*UTS_RELEASE\ \"\(.\+\)\"$/\1/p" "$LINUX_HEADERS/include/generated/utsrelease.h"
}
export -f getKernelReleaseVersion

# find modified files
modifiedFiles()
{
	git -C "$workdir" diff --name-only | grep -E ".+\.[ch]$"
}
export -f modifiedFiles

generateModuleName()
{
	local file=$1
	local crc=`cksum <<< "$file" | cut -d' ' -f1`
	crc=$( printf "%08x" $crc );
	local module="$(filenameNoExt $file)"
	local modulename=${module/-/_}
	echo deku_${crc}_$modulename
}
export -f generateModuleName

generateDEKUHash()
{
	local files=`
	find command -type f -name "*";				\
	find deploy -type f -name "*";				\
	find integration -type f -name "*";			\
	find . -maxdepth 1 -type f -name "*.sh";	\
	find . -maxdepth 1 -type f -name "*.c";		\
	echo ./deku									\
	`
	local sum=
	while read -r file; do
		sum+=`md5sum $file`
	done <<< "$files"
	sum=`md5sum <<< "$sum" | cut -d" " -f1`
	echo "$sum"
}
export -f generateDEKUHash
