package main

const (
	// Cache dir
	CACHE_DIR = "$HOME/.cache/deku"

	// Default name for workdir
	DEFAULT_WORKDIR = "workdir"

	// File with path to object file from kernel/module build directory
	FILE_OBJECT_PATH = "obj"

	// File with source file path
	FILE_SRC_PATH = "path"

	// File for note in module
	NOTE_FILE = "note"

	// Dir with kernel's object symbols
	SYMBOLS_DIR = "symbols"

	// Configuration file
	CONFIG_FILE = "$workdir/config"

	// Template for DEKU module suffix
	MODULE_SUFFIX_FILE = "resources/module_suffix_tmpl.c"

	// DEKU script to reload modules
	DEKU_RELOAD_SCRIPT = "deku_reload.sh"

	// Prefix for functions that manages DEKU
	DEKU_FUN_PREFIX = "__deku_fun_"

	// Local kernel version
	KERNEL_VERSION = "version"

	// Commands script dir
	COMMANDS_DIR = "command"

	// Kernel sources install dir
	KERN_SRC_INSTALL_DIR = ""

	// Current DEKU version hash
	DEKU_HASH = "hash"

	// File with missing symbols to relocate
	MISS_SYM = "miss_sym"

	// File with deku module dependencies
	DEPS = "deps"
)

const (
	RED    = "\x1b[31m"
	GREEN  = "\x1b[32m"
	ORANGE = "\x1b[33m"
	BLUE   = "\x1b[34m"
	NC     = "\x1b[0m" // No Color
)

type Config struct {
	buildDir          string
	crosBoard         string
	crosPath          string
	deployParams      string
	deployType        string
	ignoreCross       bool
	kernSrcInstallDir string
	linuxHeadersDir   string
	modulesDir        string
	sourceDir         string
	sshOptions        string
	systemMap         string
	useLLVM           string
	workdir           string
	isCros            bool
	kernelVersion     uint64
}
