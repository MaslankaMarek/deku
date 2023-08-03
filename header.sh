# SPDX-License-Identifier: Apache-2.0
# Copyright (C) Semihalf, 2022
# Author: Marek Maślanka <mm@semihalf.com>

# default name for workdir
export DEFAULT_WORKDIR=workdir

# file with modiefied symbols
export MOD_SYMBOLS_FILE=modsym

# file with module name where file was built-in
export FILE_OBJECT=obj

# file with source file path
export FILE_SRC_PATH=path

# file for note in module
export NOTE_FILE=note

# dir with kernel's object symbols
export SYMBOLS_DIR="$workdir/symbols"

# configuration file
export CONFIG_FILE="$workdir/config"

# template for DEKU module suffix
export MODULE_SUFFIX_FILE=module_suffix_tmpl.c

# DEKU script to reload modules
export DEKU_RELOAD_SCRIPT=deku_reload.sh

# prefix for functions that manages DEKU
export DEKU_FUN_PREFIX="__deku_fun_"

# file where kernel version is stored
export KERNEL_VERSION_FILE="$workdir/version"

# commands script dir
export COMMANDS_DIR=command

# kernel sources install dir
export KERN_SRC_INSTALL_DIR=

# log level filter
export LOG_LEVEL=1 # 0 - debug, 1 - info, 2 - warning, 3 - error

# colors
export RED='\033[0;31m'
export GREEN='\033[0;32m'
export ORANGE='\033[0;33m'
export NC='\033[0m' # No Color

# errors
export NO_ERROR=0
export ERROR_UNKNOWN=5
export ERROR_NOT_SYNCED=10
export ERROR_KLP_IS_NOT_ENABLED=11
export ERROR_INVALID_KERN_SRC_DIR=12
export ERROR_NO_DEPLOY_TYPE=13
export ERROR_NO_DEPLOY_PARAMS=14
export ERROR_INVALID_DEPLOY_TYPE=15
export ERROR_LOAD_MODULE=16
export ERROR_APPLY_KLP=17
export ERROR_CANT_FIND_AFDO=18
export ERROR_CANT_FIND_SYM_INDEX=19
export ERROR_CANT_FIND_SYMBOL=20
export ERROR_NO_WORKDIR=21
export ERROR_GENERATE_LIVEPATCH_MODULE=22
export ERROR_BUILD_MODULE=23
export ERROR_NO_SUPPORT_COLD_FUN=24
export ERROR_FORBIDDEN_MODIFY=25
export ERROR_NO_SUPPORT_MULTI_FUNC=26
export ERROR_EXTRACT_SYMBOLS=27
export ERROR_UNSUPPORTED_CHANGES=28
export ERROR_CHANGE_CALL_TO_ORIGIN=29
export ERROR_UNSUPPORTED_READ_MOSTLY=30
export ERROR_UNSUPPORTED_REF_SYM_FROM_MODULE=31
export ERROR_NO_BOARD_PARAM=32
export ERROR_INSUFFICIENT_BUILD_PARAMS=33
export ERROR_INVALID_KERNEL_ON_DEVICE=34
export ERROR_WORKDIR_EXISTS=35
export ERROR_BOARD_NOT_EXISTS=36
