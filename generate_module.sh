#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (C) Semihalf, 2022
# Author: Marek Maślanka <mm@semihalf.com>
#
# Generate livepatch module by compare elf files and extract changed functions

RUN_POST_BUILD=0

generateModuleId()
{
	local file=$1
	local deku_module_id=`git -C "$workdir" diff -W -- $file | cksum | cut -d' ' -f1`
	printf "0x%08x" $deku_module_id
}

generateMakefile()
{
	local makefile=$1
	local file=$2
	local filename=$(filenameNoExt "$file")
	local dir=`dirname $file`
	local inc="$SOURCE_DIR/$dir"

	echo "KBUILD_MODPOST_WARN = 1" > $makefile
	echo "KBUILD_CFLAGS += -ffunction-sections -fdata-sections" >> $makefile
	while true; do
		echo "EXTRA_CFLAGS += -I$inc" >> $makefile
		# include Makefiles from sources to get "ccflags-y" and other flags
		[ -f "$inc/Makefile" ] && echo "include $SOURCE_DIR/$dir/Makefile" >> "$makefile"
		[ -f "$inc/Kconfig" ] && break
		inc+="/.."
		dir+="/.."
	done

	echo '$(info ccflags-y: $(ccflags-y))' >> $makefile
	echo "obj-y :=" >> $makefile
	echo "obj-m := _$filename.o $filename.o" >> $makefile
	echo "all:" >> $makefile
	echo "	make -C $LINUX_HEADERS M=\$(PWD) modules" >> $makefile
	echo "clean:" >> $makefile
	echo "	make -C $LINUX_HEADERS M=\$(PWD) clean" >> $makefile
}

generateLivepatchMakefile()
{
	local makefile=$1
	local file=$2
	local module=$3
	local pfile=$(filenameNoExt "$module")
	local dir=`dirname $file`
	local inc="$SOURCE_DIR/$dir"

	[[ -f "$makefile" ]] && mv -f $makefile "${makefile}_modules"

	echo "KBUILD_MODPOST_WARN = 1" > $makefile
	echo "KBUILD_CFLAGS += -ffunction-sections -fdata-sections" >> $makefile

	echo "obj-m += $pfile.o" >> $makefile
	echo "$pfile-objs := livepatch.o patch.o" >> $makefile
	echo "all:" >> $makefile
	echo "	make -C $LINUX_HEADERS M=\$(PWD) modules" >> $makefile
	echo "clean:" >> $makefile
	echo "	make -C $LINUX_HEADERS M=\$(PWD) clean" >> $makefile
}

prepareToBuild()
{
	local moduledir=$1
	local basename=$2
	# add license to allow build file as module
	# TODO: check if license already exists in file
	# TODO: find license instead force use "GPL"
	echo '#include <linux/module.h>' >> "$moduledir/_$basename"
	echo '#include <linux/module.h>' >> "$moduledir/$basename"
	echo 'MODULE_LICENSE("GPL");' >> "$moduledir/_$basename"
	echo 'MODULE_LICENSE("GPL");' >> "$moduledir/$basename"
}

cmdBuildFile()
{
	local srcfile=$1
	local file="${srcfile##*/}"
	local dir=`dirname "$srcfile"`
	local cmdfile="$BUILD_DIR/$dir/.${file/.c/.o.cmd}"
	[[ ! -f $cmdfile ]] && return
	local skipparam=("-o")
	local newcmd=()
	local cmd=`head -n 1 $cmdfile`
	cmd="${cmd#*=}"
	local skiparg=0
	for opt in $cmd; do
		[[ $skiparg != 0 ]] && { skiparg=0; continue; }
		if [[ $opt == *"="* ]]; then
			local param="${opt%%=*}"
			local arg="${opt#*=}"
			[[ " ${skipparam[*]} " =~ " ${param} " ]] && continue
		else
			[[ " ${skipparam[*]} " =~ " ${opt} " ]] && { skiparg=1; continue; }
		fi
		newcmd+=("$opt")
	done
	unset 'newcmd[${#newcmd[@]}-1]'
	newcmd+=("-I$SOURCE_DIR/$dir")
	echo "${newcmd[*]}"
}

buildFile()
{
	local srcfile=$1
	local compilefile=$2
	local outfile=$3
	local separatesections=1
	cmd=$(cmdBuildFile "$srcfile")
	[[ $cmd == "" ]] && { logInfo "Can't find command to build $srcfile"; return 1; }
	[[ $outfile != /* ]] && outfile="`pwd`/$outfile"
	[[ $compilefile != /* ]] && compilefile="`pwd`/$compilefile"
	[[ $separatesections != 0 ]] && cmd+=" -ffunction-sections -fdata-sections"
	cmd+=" -o $outfile $compilefile"
	cd "$LINUX_HEADERS"
	eval "$cmd"
	local res=$?
	cd $OLDPWD
	[[ $res != 0 ]] && logInfo "Failed to build $srcfile"
	return $res
}

buildModules()
{
	local moduledir=$1
	# go to workdir instead of use "-C" because the "$(PWD)" is used in Makefile
	cd $moduledir
	out=`make $USE_LLVM 2>&1`
	rc=$?
	cd $OLDPWD

	local filelog="$moduledir/build.log"
	echo -e "$out" > "$filelog"

	if [ $rc != 0 ]; then
		regexerr='^.\+\(\/\w\+\.\w\+\):\([0-9]\+\):[0-9]\+:.* error: \(.\+\)'
		local errorcatched=0
		while read -r line;
		do
			err=`echo $line | sed -n "s/$regexerr/\1\|\2/p"`
			if [ -n "$err" ]; then
				local file=`echo $line | sed "s/$regexerr/\1/" | tr -d "//"`
				local no=`echo $line | sed "s/$regexerr/\2/"`
				local err=`echo $line | sed "s/$regexerr/\3/"`
				echo -e "$file:$no ${RED}error:${NC} $err. See more: $filelog"
				errorcatched=1
			fi
		done <<< "$out"

		if [ $errorcatched == 0 ]; then
			logErr "Error:"
			while read -r line; do
				echo $line
			done <<< "$out"
		fi
		exit $rc
	fi
}

buildLivepatchModule()
{
	local moduledir=$1
	local filelog="$moduledir/build.log"

	[[ -f "$filelog" ]] && mv -f $filelog "$moduledir/build_modules.log"
	buildModules "$moduledir"
}

generateDiffObject()
{
	local moduledir=$1
	local file=$2
	local filename=$(filenameNoExt "$file")
	local out=`./elfutils --diff -a "$moduledir/_$filename.o" -b "$moduledir/$filename.o"`
	local tmpmodfun=`sed -n "s/^Modified function: \(.\+\)/\1/p" <<< "$out"`
	local newfun=`sed -n "s/^New function: \(.\+\)/\1/p" <<< "$out"`
	local modfun=()

	while read -r fun;
	do
		local initfunc=`objdump -t -j ".init.text" "$moduledir/_$filename.o" 2>/dev/null | grep "\b$fun\b"`
		if [[ "$initfunc" != "" ]]; then
			logInfo "Detect modifications in the init '$fun' function. Changes from this function won't be applied."
			continue
		fi
		modfun+=("$fun")
	done <<< "$tmpmodfun"

	printf "%s\n" "${modfun[@]}" > "$moduledir/$MOD_SYMBOLS_FILE"

	[[ "$out" == "" ]] && return 0;

	local originfuncs=`nm -C -f posix "$BUILD_DIR/${file%.*}.o" | grep -i " t " | cut -d ' ' -f 1`
	local extractsyms=""
	for fun in ${modfun[@]};
	do
		[[ "$fun" == "" ]] && continue
		# if modified function is inlined in origin file then get functions that call
		# this function and make DEKU module for those functions
		if ! grep -q "\b$fun\b" <<< "$originfuncs"; then
			logDebug "$fun function in $file is inlined"
			sed -i "/\b$fun\b/d" "$moduledir/$MOD_SYMBOLS_FILE"
			local calls=`./elfutils --callchain -f "$moduledir/$filename.o" | grep -E "^\b$fun\b"`
			while read -r chain;
			do
				# check if the call chain contains only one function from origin file
				local count=0
				local originfun=""
				local toextract=""
				for s in $chain; do
					if grep -q "\b$s\b" <<< "$originfuncs"; then
						((count=count+1))
						[[ $count > 1 ]] && break
						originfun=$s
					fi
					# add to list if another function in call chain is inlined
					[[ $count == 0 ]] && ! grep -q "\b$s\b" <<< "$extractsyms" && toextract+="-s $s "
				done
				if [[ $count == 1 ]]; then
					extractsyms+="$toextract"
					extractsyms+="-s $originfun "
					echo "$originfun" >> "$moduledir/$MOD_SYMBOLS_FILE"
				fi
			done <<< "$calls"
		else
			extractsyms+="-s $fun "
		fi
	done

	while read -r fun;
	do
		[[ "$fun" == "" ]] && continue
		extractsyms+="-s $fun "
	done <<< "$newfun"
	./elfutils --extract -f "$moduledir/$filename.o" -o "$moduledir/patch.o" $extractsyms
	[[ $? != 0 ]] && { logErr "Failed to extract modified symbols"; exit 1; }
}

generateLivepatchSource()
{
	local moduledir=$1
	local file=$2
	local outfile="$moduledir/livepatch.c"
	local modsymfile="$moduledir/$MOD_SYMBOLS_FILE"
	local klpfunc=""
	local prototypes=""

	local objname

	# find object for modified functions
	while read -r symbol; do
		objname=$(findObjWithSymbol $symbol "$file")
		[ ! -z "$objname" ] && { echo $objname > "$moduledir/$FILE_OBJECT"; break; }
	done < $modsymfile
	if [ -z "$objname" ]; then
		logWarn "Modified file '$file' is not compiled into kernel/module. Skip the file"
		return 1
	fi

	while read -r symbol; do
		local plainsymbol="${symbol//./_}" 
		# fill list of a klp_func struct
		klpfunc="$klpfunc		{
			.old_name = \"${symbol}\",
			.new_func = $DEKU_FUN_PREFIX${plainsymbol},
		},"

		prototypes="$prototypes
			void $DEKU_FUN_PREFIX$plainsymbol(void);"
	done < $modsymfile

	local klpobjname
	if [ $objname = "vmlinux" ]; then
		klpobjname="NULL"
	else
		klpobjname="\"$objname\""
	fi

	# add to module necessary headers
	echo >> $outfile
	cat >> $outfile <<- EOM
	#include <linux/kernel.h>
	#include <linux/module.h>
	#include <linux/livepatch.h>
	#include <linux/version.h>
	EOM

	# add livepatching code
	cat >> $outfile <<- EOM
	$prototypes

	static struct klp_func deku_funcs[] = {
	$klpfunc { }
	};

	static struct klp_object deku_objs[] = {
		{
			.name = $klpobjname,
			.funcs = deku_funcs,
		}, { }
	};

	static struct klp_patch deku_patch = {
		.mod = THIS_MODULE,
		.objs = deku_objs,
	};
	EOM
	cat $MODULE_SUFFIX_FILE >> $outfile
}

postBuild()
{
	[[ $RUN_POST_BUILD != 1 ]] && return
	if [[ "$POST_BUILD" != "" ]]; then
		logDebug "Run postbuild: $POST_BUILD"
		eval "$POST_BUILD"
		RUN_POST_BUILD=0
	fi
}

buildInKernel()
{
	local file=$1
	[ -f "$BUILD_DIR/${file%.*}.o" ] && return 0
	return 1
}

main()
{
	local files=$(modifiedFiles)
	if [ -z "$files" ]; then
		# No modification detected
		exit
	fi

	for file in $files
	do
		if [[ "${file#*.}" != "c" ]]; then
			logWarn "Only changes in '.c' files are supported. Undo changes in $file and try again."
			exit 1
		fi
	done

	if [[ "$PRE_BUILD" != "" ]]; then
		logDebug "Run prebuild: $PRE_BUILD"
		eval "$PRE_BUILD"
		RUN_POST_BUILD=1
	fi

	for file in $files
	do
		local basename=`basename $file`
		local filename=$(filenameNoExt "$file")
		if ! buildInKernel "$file"; then
			logWarn "File '$file' is not used in the kernel or module. Skip"
			continue
		fi
		local module=$(generateModuleName "$file")
		local moduledir="$workdir/$module"
		local moduleid=$(generateModuleId "$file")
		# check if changed since last run
		if [ -s "$moduledir/id" ]; then
			local prev=$(<$moduledir/id)
			[ "$prev" ==  "$moduleid" ] && continue
		fi

		rm -rf $moduledir
		mkdir $moduledir

		# write diff to file for debug purpose
		git -C $workdir diff -- $file > "$moduledir/diff"

		# file name with prefix '_' is the origin file
		git -C $workdir cat-file blob ":$file" > "$moduledir/_$basename"
		cp "$SOURCE_DIR/$file" "$moduledir/$basename"
		echo -n "$file" > "$moduledir/$FILE_SRC_PATH"

		local usekbuild=0
		buildFile $file "$moduledir/_$basename" "$moduledir/_$filename.o"
		usekbuild=$?
		if [[ $usekbuild == 0 ]]; then
			buildFile $file "$moduledir/$basename" "$moduledir/$filename.o"
			usekbuild=$?
		fi

		if [[ $usekbuild != 0 ]]; then
			logInfo "Use kbuild to build modules"
			generateMakefile "$moduledir/Makefile" "$file"

			prepareToBuild "$moduledir" "$basename"
			buildModules "$moduledir"
		fi

		if generateDiffObject "$moduledir" "$file"; then
			logInfo "No valid changes found in '$file'"
			continue
		fi

		generateLivepatchSource "$moduledir" "$file" || continue
		generateLivepatchMakefile "$moduledir/Makefile" "$file" "$module"
		buildLivepatchModule "$moduledir"

		# restore calls to origin func XYZ instead of __deku_XYZ
		while read -r symbol; do
			local plainsymbol="${symbol//./_}"
			./elfutils --changeCallSymbol -s ${DEKU_FUN_PREFIX}${plainsymbol} -d ${plainsymbol} \
					   "$moduledir/$module.ko" || ext 1
			objcopy --strip-symbol=${DEKU_FUN_PREFIX}${plainsymbol} "$moduledir/$module.ko"
		done < "$moduledir/$MOD_SYMBOLS_FILE"

		echo -n "$moduleid" > "$moduledir/id"

		# Add note to module with module name and id
		local notefile="$moduledir/$NOTE_FILE"
		echo -n "$module " > "$notefile"
		cat "$moduledir/id" >> "$notefile"
		echo "" >> "$notefile"
		objcopy --add-section .note.deku="$notefile" \
				--set-section-flags .note.deku=alloc,readonly \
				"$moduledir/$module.ko"

	done
	postBuild
}

trap postBuild EXIT

main $@
