#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (C) Semihalf, 2022
# Author: Marek Maślanka <mm@semihalf.com>

# Functions return "0" as success

WORKDIR="workdir_test"
export workdir="$WORKDIR"
. ./header.sh

QEMU_SSH_PORT="60022"
SSHPARAMS="root@localhost -p $QEMU_SSH_PORT -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -i test/testing_rsa"
DEPLOY_PARAMS="root@localhost:$QEMU_SSH_PORT -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -i test/testing_rsa"
REMOTE_OUT=""

TEST_CACHE_DIR="$HOME/.cache/deku"
ROOTFS_IMG=test/rootfs.img

SOURCE_DIR="$TEST_CACHE_DIR/linux"
BUILD_DIR="$TEST_CACHE_DIR/build-linux-deku"
MAIN_PATH=""
KERNEL_VERSION="v5.15"

appendToFunctionAt()
{
	local file=$1
	local line=$2
	local text=$3
	sed -i "$line,/{/ s/{/{\n\t$text/g" $file
}

modifyFunctions()
{
	local file=$1
	local pass=$2
	echo "Modify file ($pass): $file"
	local out=`ctags -x -u --c-kinds=+p --fields=+afmikKlnsStz --extra=+q $file`
	local functions=`echo "$out" | grep function`
	[[ $functions == "" ]] && { echo "No functions found in $file. Skip"; return; }
	local bckIFS=$IFS
	IFS=$'\n' functions=($functions)
	IFS=$bckIFS
	local count=${#functions[@]}
	local index=0
	[[ "$pass" == "0" ]] && index=0
	[[ "$pass" == "-1" ]] && index=$((count-1))
	[[ "$pass" == "-2" ]] && index=$((count/2))
	local funX=${functions[index]}
	local arr=($funX)
	appendToFunctionAt $file ${arr[2]} 'pr_err("DEKU");'
}

appendToFunction()
{
	local file=$1
	local function=$2
	local text=$3
	echo "Append '$text' to function '$function' in file $file"
	local out=`ctags -x -u --c-kinds=f $file`
	local lineno=`sed -n "s/^$function[ ]\+function[ ]\+\([0-9]\+\)[ ]\+.\+/\1/p" <<< "$out"`
	appendToFunctionAt "$file" "$lineno" "$text"
}

runQemu()
{
	local KERNEL_IMAGE="$BUILD_DIR/arch/x86/boot/bzImage"
	local cmdline="console=ttyS0 root=/dev/sda rw"
	killall -q -9 qemu-system-x86_64
	sleep 1 # to avoid error: Could not set up host forwarding rule 'tcp::60022-:22'
	qemu-system-x86_64 -kernel "$KERNEL_IMAGE" -drive "format=raw,file=$ROOTFS_IMG" -append "'$cmdline'" \
					   -s -nic "user,hostfwd=tcp::$QEMU_SSH_PORT-:22" -daemonize
	for i in {1..150}; do
		ssh $SSHPARAMS -o ConnectTimeout=1 -q exit
		[[ $? == 0 ]] && break
		[ $(expr $i % 10) == 0 ] && echo "Waiting for qemu..."
		sleep 1
	done
}

remoteSh()
{
  REMOTE_OUT=$(ssh $SSHPARAMS "$@")
  return ${PIPESTATUS[0]}
}

prepareKernelSources()
{
	git clone git://git.kernel.org/pub/scm/linux/kernel/git/stable/linux-stable.git "$SOURCE_DIR"
	mkdir -p "$BUILD_DIR"
}

workdirContainsOnly()
{
	local notcontains=()
	local modules=`find "$WORKDIR" -type d -name deku_*`
	for i in "$@"; do
		[ ! -f "$WORKDIR/$i/$i.ko" ] && notcontains+="$i "
		modules=`sed "/^$WORKDIR\/$i\$/d" <<< "$modules"`
	done
	[[ $modules != "" ]] && >&2 echo -e "${RED}Workdir contains unexpected modules:$modules${NC}"
	((${#notcontains[@]} > 0)) && >&2 echo -e "${RED}Workdir does not contains expected modules:${notcontains[@]}${NC}"
	[[ "$modules" != "" ]] || ((${#notcontains[@]} > 0)) && return 1
	echo "Workdir contains only modules '$@'... OK"
	return 0
}

checkIfWorkdirIsEmpty()
{
	local workdirmodules=`find "$WORKDIR" -type d -name deku_*`
	[[ "$workdirmodules" != "" ]] && { >&2 echo -e "${RED}Workdir must not contain any modules. Modules in workdir: $workdirmodules${NC}"; return 1; }
	echo "Workdir is empty... OK"
	return 0
}

checkIfDmesgContains()
{
	local res=0
	remoteSh dmesg
	for i in "$@"; do
		if ! grep -q "$i" <<< "$REMOTE_OUT"; then
			>&2 echo -e "${RED}dmesg on remote does not contains:$i${NC}"
			echo "========================================================"
			echo "$REMOTE_OUT"
			echo "========================================================"
			res=1
		fi
	done
	((res == 0)) && echo "dmesg contains: '$@'... OK"
	return $res
}

checkIfDmesgNOTContains()
{
	local res=0
	remoteSh dmesg
	for i in "$@"; do
		if grep -q "$i" <<< "$REMOTE_OUT"; then
			>&2 echo -e "${RED}dmesg on remote contains unexpected:$i${NC}"
			echo "========================================================"
			echo "$REMOTE_OUT"
			echo "========================================================"
			res=1
		fi
	done
	((res == 0)) && echo "dmesg not contains: '$@'... OK"
	return $res
}

checkIfFileExists()
{
	local file=$1
	[[ ! -f "$file" ]] && { >&2 echo -e "${RED}File '$file' does not exists${NC}"; return 1; }
	echo "Found '$file'... OK"
	return 0
}

buildKernel()
{
	rm -f "$BUILD_DIR/vmlinux"
	yes "" | make -C "$SOURCE_DIR" O="$BUILD_DIR" oldconfig
	make -C "$SOURCE_DIR" O="$BUILD_DIR" -j`nproc`
}

enableKernelConfig()
{
	local flag=$1
	local action="--enable"
	[[ $2 != "" ]] && action=$2
	"$SOURCE_DIR"/scripts/config --file "$BUILD_DIR"/.config $action $flag
}

# TODO: Support "allyesconfig" as in "prepareKernelOld"
prepareKernel()
{
	local version=$1
	git -C "$SOURCE_DIR" reset --hard
	git -C "$SOURCE_DIR" clean -d -f
	git -C "$SOURCE_DIR" checkout $version

	make -C "$SOURCE_DIR" O="$BUILD_DIR" defconfig
	sed -i s/=m/=y/g "$BUILD_DIR/.config"
	enableKernelConfig KALLSYMS_ALL
	enableKernelConfig FUNCTION_TRACER
	enableKernelConfig LIVEPATCH

	buildKernel
}

prepareKernelOld()
{
	local version=$1
	local allyesconfig=$2
	git -C "$SOURCE_DIR" reset --hard
	git -C "$SOURCE_DIR" checkout $version

	if [[ $allyesconfig != "1" ]]; then
		local kernelconfig=kernelconfig4
		[[ $version != v4.* ]] && kernelconfig=kernelconfig5
		echo "Use config: $kernelconfig"
		cp "$MAIN_PATH/$kernelconfig" "$BUILD_DIR/.config"
	else
		echo "Use config: allyesconfig"
		make -C "$SOURCE_DIR" allyesconfig
		cp "$SOURCE_DIR/.config" "$BUILD_DIR/.config"
	fi
	make -C "$SOURCE_DIR" mrproper
	yes "" | make -C "$SOURCE_DIR" O="$BUILD_DIR" oldconfig
	buildKernel
}

integrationTest()
{
	local text='pr_info("tcp_v4_connect test\\n");'
	local text2='pr_info("tcp_v4_connect test2\\n");'

	prepareKernel $KERNEL_VERSION

	runQemu

	rm -rf "$WORKDIR"
	./deku -w "$WORKDIR" -b "$BUILD_DIR" -d ssh -p "$DEPLOY_PARAMS" init

	# check if no module is build
	./deku -w "$WORKDIR" deploy
	checkIfWorkdirIsEmpty || return 1

	# check simple modification
	appendToFunction "$SOURCE_DIR/net/ipv4/tcp_ipv4.c" tcp_v4_connect "$text"

	remoteSh "dmesg --clear"
	./deku -w "$WORKDIR" deploy
	workdirContainsOnly deku_47910166_tcp_ipv4 || return 2
	sleep 1
	remoteSh wget www.google.com -O /dev/null 2>/dev/null
	sleep 1
	checkIfDmesgContains "tcp_v4_connect test" || return 3

	echo -e "${GREEN}------------------------- INTEGRATION TEST 1 DONE -------------------------${NC}"

	# check whether the same changes in source code generates the same module id and prevents to upload
	rm -rf "$WORKDIR/deku_47910166_tcp_ipv4"
	remoteSh "dmesg --clear"
	./deku -w "$WORKDIR" deploy
	sleep 1
	checkIfDmesgNOTContains "livepatch: enabling patch 'deku_47910166_tcp_ipv4'" || return 4

	echo -e "${GREEN}------------------------- INTEGRATION TEST 2 DONE -------------------------${NC}"

	# check scenario when user build kernel and sync is performed
	buildKernel
	runQemu

	./deku -w "$WORKDIR" sync
	checkIfWorkdirIsEmpty || return 5
	remoteSh "dmesg --clear"
	./deku -w "$WORKDIR" deploy
	checkIfWorkdirIsEmpty || return 6
	remoteSh wget www.google.com -O /dev/null 2>/dev/null
	sleep 5
	checkIfDmesgContains "tcp_v4_connect test" || return 7

	echo -e "${GREEN}------------------------- INTEGRATION TEST 3 DONE-------------------------${NC}"

	# check scenario when modify detection works after sync
	appendToFunction "$SOURCE_DIR/net/ipv4/tcp_ipv4.c" tcp_v4_connect "$text2"

	remoteSh "dmesg --clear"
	./deku -w "$WORKDIR" deploy
	workdirContainsOnly deku_47910166_tcp_ipv4 || return 8
	checkIfDmesgContains "livepatch: enabling patch 'deku_47910166_tcp_ipv4'" || return 9
	remoteSh wget www.google.com -O /dev/null 2>/dev/null
	sleep 5
	checkIfDmesgContains "tcp_v4_connect test2" || return 10

	echo -e "${GREEN}------------------------- INTEGRATION TEST 4 DONE -------------------------${NC}"

	# check scenario where module must be unloaded due to undo all modifications
	git -C "$SOURCE_DIR" reset --hard
	buildKernel
	runQemu
	./deku -w "$WORKDIR" sync
	sleep 15
	appendToFunction "$SOURCE_DIR/net/ipv4/tcp_ipv4.c" tcp_v4_connect "$text"
	./deku -w "$WORKDIR" deploy
	remoteSh "dmesg --clear"
	sleep 5
	remoteSh wget www.google.com -O /dev/null 2>/dev/null
	sleep 15
	checkIfDmesgContains "tcp_v4_connect test" || return 11
	git -C "$SOURCE_DIR" reset --hard
	yes "y" | ./deku -w "$WORKDIR" deploy
	remoteSh "dmesg --clear"
	remoteSh wget www.google.com -O /dev/null 2>/dev/null
	sleep 5
	checkIfDmesgNOTContains "tcp_v4_connect test" || return 12

	echo -e "${GREEN}------------------------- INTEGRATION TEST 5 DONE -------------------------${NC}"

	return 0
}

compareFileContents()
{
	local file=$1
	local expected=$2
	local got=$(<$file)
	if [[ $expected != $got ]]; then
		>&2 echo -e "${RED}Unexpected contents of file: $file${NC}"
		echo "================== EXPECTED =================="
		echo -e "${expected}"
		echo "=============================================="
		echo ""
		echo "===================== GOT ===================="
		echo -e "${got}"
		echo "=============================================="
		echo ""
		return 1
	fi
	echo "Contents of file $file is... OK"
	return 0
}

# modify inline function and check if it is properly detected
inlineTest()
{
	prepareKernel v5.4.200

	rm -rf "$WORKDIR"
	./deku -w "$WORKDIR" -b "$BUILD_DIR" -d ssh -p "$DEPLOY_PARAMS" init

	appendToFunction "$SOURCE_DIR/drivers/gpu/drm/i915/display/intel_dvo.c" intel_attached_dvo "pr_info(\"\");"
	./deku -w "$WORKDIR" build || return 1
	local expected='intel_dvo_mode_valid
intel_dvo_connector_get_hw_state
intel_dvo_detect'
	compareFileContents "$WORKDIR/deku_dc0fef60_intel_dvo/$MOD_SYMBOLS_FILE" "$expected" || return 2
	echo -e "${GREEN}------------------------- INLINE TEST DONE -------------------------${NC}"
	return 0
}

# modify inline function and check if it is properly detected
inline2Test()
{
	prepareKernel v5.4.200

	rm -rf "$WORKDIR"
	./deku -w "$WORKDIR" -b "$BUILD_DIR" -d ssh -p "$DEPLOY_PARAMS" init

	appendToFunction "$SOURCE_DIR/drivers/input/evdev.c" evdev_get_mask_cnt "pr_info(\"evdev_get_mask_cnt\");" # evdev_set_mask evdev_get_mask __evdev_is_filtered
	appendToFunction "$SOURCE_DIR/drivers/input/evdev.c" __evdev_is_filtered "pr_info(\"__evdev_is_filtered\");" # evdev_pass_values
	appendToFunction "$SOURCE_DIR/drivers/input/evdev.c" evdev_pass_values "pr_info(\"evdev_pass_values\");" # evdev_events
	./deku -w "$WORKDIR" build || return 1
	local expected='evdev_do_ioctl
evdev_pass_values
evdev_events
evdev_event'
	compareFileContents "$WORKDIR/deku_e8db891b_evdev/$MOD_SYMBOLS_FILE" "$expected" || return 2
	echo -e "${GREEN}------------------------- INLINE2 TEST DONE -------------------------${NC}"
	return 0
}

# test build-in module
moduleTest()
{
	prepareKernel v5.4.200

	enableKernelConfig NF_LOG_ARP "--module"
	buildKernel
	rm -rf "$WORKDIR"
	./deku -w "$WORKDIR" -b "$BUILD_DIR" -d ssh -p "$DEPLOY_PARAMS" init

	appendToFunction "$SOURCE_DIR/net/ipv4/netfilter/nf_log_arp.c" nf_log_arp_packet "pr_info(\"\");"
	./deku -w "$WORKDIR" build || return 1
	local expected='nf_log_arp_packet'
	compareFileContents "$WORKDIR/deku_29b5b22d_nf_log_arp/$MOD_SYMBOLS_FILE" "$expected" || return 2
	expected='nf_log_arp'
	compareFileContents "$WORKDIR/deku_29b5b22d_nf_log_arp/$FILE_OBJECT" "$expected" || return 3
	echo -e "${GREEN}------------------------- MODULE TEST DONE -------------------------${NC}"
	return 0
}

# check if symbols are properly generated
symbolsTest()
{
	prepareKernel v5.15

	enableKernelConfig X86_PKG_TEMP_THERMAL "--module"
	buildKernel
	rm -rf "$WORKDIR"
	./deku -w "$WORKDIR" -b "$BUILD_DIR" -d ssh -p "$DEPLOY_PARAMS" init

	appendToFunction "$SOURCE_DIR/drivers/thermal/intel/x86_pkg_temp_thermal.c" pkg_thermal_cpu_offline "pr_info(\"x86_pkg_temp_thermal\");"
	./deku -w "$WORKDIR" build || return 1
	checkIfFileExists "$SYMBOLS_DIR/drivers/thermal/intel/x86_pkg_temp_thermal" || return 2
	./deku -w "$WORKDIR" sync
	checkIfFileExists "$SYMBOLS_DIR/drivers/thermal/intel/x86_pkg_temp_thermal" || return 3
	echo -e "${GREEN}------------------------- SYMBOLS TEST DONE -------------------------${NC}"
	return 0
}

# check if driver that is complex - multi-file/dir - is build properly
complexTest()
{
	prepareKernel "v5.4.200"

	rm -rf "$WORKDIR"
	enableKernelConfig CONFIG_IWLWIFI
	buildKernel

	./deku -w "$WORKDIR" -b "$BUILD_DIR" -d ssh -p "$DEPLOY_PARAMS" init
	appendToFunction "$SOURCE_DIR/drivers/net/wireless/intel/iwlwifi/pcie/trans.c" iwl_trans_pcie_grab_nic_access "pr_info(\"iwl_trans_pcie_grab_nic_access\");"
	./deku -w "$WORKDIR" build || return 1
	checkIfFileExists "$WORKDIR/deku_72aff690_trans/deku_72aff690_trans.ko" || return 2

	appendToFunction "$SOURCE_DIR/drivers/acpi/acpica/evxfgpe.c" acpi_update_all_gpes "pr_info(\"\");"
	./deku -w "$WORKDIR" build || return 3
	checkIfFileExists "$WORKDIR/deku_3890c8df_evxfgpe/deku_3890c8df_evxfgpe.ko" || return 4

	echo -e "${GREEN}------------------------- COMPLEX TEST DONE -------------------------${NC}"
	return 0
}

# modify almost every file in specific dir and check if the files can be build
buildTest()
{
	local dir="$1"

	prepareKernel v5.0 1

	rm -rf "$WORKDIR"
	./deku -w "$WORKDIR" -b "$BUILD_DIR" -d ssh -p "$DEPLOY_PARAMS" init

	local out=`find $dir -maxdepth 30 -type f -name "*.c"`
	while read -r file; do
		# err ctag?
		[[ "`basename $file`" == "tc358767.c" ]] && continue
		[[ "`basename $file`" == "dvo_ns2501.c" ]] && continue
		[[ "`basename $file`" == "drm_encoder_slave.c" ]] && continue
		[[ "`basename $file`" == "intel_tv.c" ]] && continue
		[[ "`basename $file`" == "mkregtable.c" ]] && continue

		# array_size
		[[ "`basename $file`" == "libata-transport.c" ]] && continue

		# support DECLARE_WORK
		[[ "`basename $file`" == "dd.c" ]] && continue

		# preserve global variable
		[[ "`basename $file`" == "devcon.c" ]] && continue

		# #ifdef
		[[ "`basename $file`" == "platform-msi.c" ]] && continue

		# empty line after multiline macro
		[[ "`basename $file`" == "uncore_snbep.c" ]] && continue

		# # gcc invalid copy
		# [[ "`basename $file`" == "ch7006_mode.c" ]] && continue

		# todo: better detect array declaration used in ARRAY_SIZE
		[[ "`basename $file`" == "intel_uncore.c" ]] && continue

		# todo: check if symbol exists in build object and support for uncomment variables
		[[ "`basename $file`" == "i915_cmd_parser.c" ]] && continue

		#todo:
		[[ "`basename $file`" == "genhd.c" ]] && continue

		# gcc error
		[[ "$file" == *arch/x86/kvm/emulate.c ]] && continue

		# not buildable even as plain modules
		[[ "`basename $file`" == "kmmio.c" ]] && continue
		# only inlined functoins ?
		[[ "`basename $file`" == "a20.c" ]] && continue

		# can't resolve symbol
		[[ "$file" == *drivers/gpu/drm/drm_dp_aux_dev.c ]] && continue

		# restore variable instead a function
		[[ "$file" == *drivers/gpu/drm/gma500/mdfld_intel_display.c ]] && continue

		# unusual code
		[[ "$file" == *drivers/gpu/drm/drm_connector.c ]] && continue
		[[ "$file" == *drivers/gpu/drm/i915/i915_gem.c ]] && continue

		# lack of include in Makefile
		[[ "$file" == *drivers/gpu/drm/nouveau/nouveau_abi16.c ]] && continue
		[[ "$file" == *drivers/gpu/drm/nouveau/nouveau_acpi.c ]] && continue
		[[ "$file" == *drivers/gpu/drm/nouveau/nouveau_* ]] && continue
		[[ "$file" == *drivers/gpu/drm/nouveau/nv04_fbcon.c ]] && continue
		[[ "$file" == *drivers/gpu/drm/nouveau/nv04_fence.c ]] && continue
		[[ "$file" == *drivers/gpu/drm/nouveau/nv04_* ]] && continue
		[[ "$file" == *drivers/gpu/drm/nouveau/nv10_* ]] && continue
		[[ "$file" == *drivers/gpu/drm/nouveau/nv* ]] && continue
		[[ "$file" == *drivers/gpu/drm/radeon/evergreen_cs.c ]] && continue
		[[ "$file" == *drivers/gpu/drm/radeon/r100.c ]] && continue
		[[ "$file" == *drivers/gpu/drm/radeon/* ]] && continue
		[[ "$file" == *drivers/gpu/drm/vmwgfx/vmwgfx_blit.c ]] && continue
		[[ "$file" == *drivers/gpu/drm/vmwgfx/vmwgfx_msg.c ]] && continue
		[[ "$file" == *drivers/gpu/drm/vmwgfx/vmwgfx_* ]] && continue

		# enhance the test's 'modifyfunction' (#ifdef)
		[[ "$file" == *drivers/gpu/drm/drm_gem_cma_helper.c ]] && continue
		[[ "$file" == *drivers/gpu/drm/gma500/mdfld_dsi_output.c ]] && continue

		# conflict with livepatching var name
		[[ "$file" == *drivers/gpu/drm/bridge/lvds-encoder.c ]] && continue

		# acpi
		[[ "`basename $file`" == "acpi_lpit.c" ]] && continue
		[[ "`basename $file`" == "dsdebug.c" ]] && continue
		[[ "`basename $file`" == "evxface.c" ]] && continue
		[[ "`basename $file`" == "hwsleep.c" ]] && continue
		[[ "`basename $file`" == "hwvalid.c" ]] && continue
		[[ "`basename $file`" == "hwxfsleep.c" ]] && continue
		[[ "`basename $file`" == "nsload.c" ]] && continue
		[[ "`basename $file`" == "nsrepair2.c" ]] && continue
		[[ "`basename $file`" == "utosi.c" ]] && continue
		[[ "`basename $file`" == "utpredef.c" ]] && continue
		[[ "`basename $file`" == "battery.c" ]] && continue
		[[ "`basename $file`" == "button.c" ]] && continue
		[[ "`basename $file`" == "device_sysfs.c" ]] && continue
		[[ "`basename $file`" == "osi.c" ]] && continue
		[[ "`basename $file`" == "osl.c" ]] && continue
		[[ "`basename $file`" == "processor_perflib.c" ]] && continue
		[[ "`basename $file`" == "scan.c" ]] && continue
		[[ "`basename $file`" == "tables.c" ]] && continue

		modifyFunctions "$file" 0
		modifyFunctions "$file" -2
		modifyFunctions "$file" -1
	done <<< "$out"

	./deku -w "$WORKDIR" build
	[ $? -ne 0 ] && return 1

	return 0
}

# test/test.sh integration
# test/test.sh inline
# test/test.sh symbols
main()
{
	MAIN_PATH=`dirname "$0"`

	local res=0
	local testname="$1"
	if [[ $res == 0 ]] && [[ "$1" == "complex" || "$1" == "all" ]]; then
		testname="Complex"
		[ ! -d "$SOURCE_DIR" ] && prepareKernelSources
		complexTest
		res=$?
	fi
	if [[ $res == 0 ]] && [[ "$1" == "inline" || "$1" == "all" ]]; then
		testname="Inline"
		[ ! -d "$SOURCE_DIR" ] && prepareKernelSources
		inlineTest
		res=$?
		[[ $res == 0 ]] && { testname="Inline 2"; inline2Test; res=$?; }
	fi
	if [[ $res == 0 ]] && [[ "$1" == "symbols" || "$1" == "all" ]]; then
		testname="Symbols"
		[ ! -d "$SOURCE_DIR" ] && prepareKernelSources
		symbolsTest
		res=$?
	fi
	if [[ $res == 0 ]] && [[ "$1" == "module" || "$1" == "all" ]]; then
		testname="Module"
		[ ! -d "$SOURCE_DIR" ] && prepareKernelSources
		moduleTest
		res=$?
	fi
	if [[ $res == 0 ]] && [[ "$1" == "integration" || "$1" == "all" ]]; then
		if [[ ! -f "$ROOTFS_IMG" ]]; then
			echo "Rootfs image is not found. Go to 'test' directory and run 'sudo ./mkrootfs.sh' to generate image."
			exit 1
		fi

		testname="Integration"
		[ ! -d "$SOURCE_DIR" ] && prepareKernelSources
		killall -q -9 qemu-system-x86_64
		integrationTest
		res=$?
		[[ $res != 0 ]] && >&2 echo -e "${RED}INTEGRATION TEST FAILED WITH ERROR CODE: $res${NC}"
	fi
	if [[ $res == 0 && "$1" == "dir" ]]; then
		testname="Multi changes"
		[ ! -d "$SOURCE_DIR" ] && prepareKernelSources
		buildTest "$SOURCE_DIR/"
		res=$?
	fi
	if (($res != 0)); then
		>&2 echo -e "${RED}!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!${NC}"
		>&2 echo -e "${RED}!!!!!!!!!!!!!!!!!!!!! ERROR !!!!!!!!!!!!!!!!!!!!!!${NC}"
		>&2 echo -e "${RED}!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!${NC}"
		>&2 echo -e "${RED}Test: $testname failed!${NC}"
	else
		echo -e "${GREEN}@@@@@@@@@@@@@@@@@ TESTS COMPLETED SUCCESSFUL @@@@@@@@@@@@@@@@@${NC}"
	fi
	exit

	if [ -f "$1" ]; then
		local file="$1"
		modifyFunctions "$file"
	fi
}

main $@
