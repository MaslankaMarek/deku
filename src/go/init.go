package main

import (
	"bytes"
	"encoding/hex"
	"errors"
	"hash/crc32"
	"os"
	"path/filepath"
	"strings"
)

type Init struct {
	params map[string]string
	config Config
}

var cacheDir = "." // TODO

func (init *Init) getParam(long, short string) string {
	var value string
	if val, ok := init.params[long]; ok {
		value = val
	} else if val, ok := init.params[short]; ok {
		value = val
	}
	var directories = []string{"builddir", "sourcesdir", "src_inst_dir", "workdir", "cros_sdk"}
	if slicesContains(directories, long) && len(value) > 0 {
		if !filepath.IsAbs(value) {
			if strings.HasPrefix(value, "~/") {
				homeDir, _ := os.UserHomeDir()
				value = filepath.Join(homeDir, value[2:])
			} else {
				value = filepath.Join(cacheDir, value) + "/"
			}
		}
		if !strings.HasSuffix(value, "/") {
			value += "/"
		}
	}
	return value
}

func (init *Init) parseParameters() {
	var noValueParams = []string{"--ignore_cros", "-v"}

	init.params = make(map[string]string)
	for i := 1; i < len(os.Args); i++ {
		opt := os.Args[i]
		value := ""
		if slicesContains(noValueParams, opt) {
			value = "1"
		} else if strings.Contains(opt, "=") {
			arr := strings.SplitN(opt, "=", 2)
			opt = arr[0]
			value = arr[1]
		} else if i+1 < len(os.Args) {
			value = os.Args[i+1]
			i++
		}
		opt = strings.TrimPrefix(opt, "-")
		opt = strings.TrimPrefix(opt, "-")
		init.params[opt] = value
	}
}

func (init *Init) getConfig() Config {
	init.parseParameters()

	var config Config
	config.buildDir = init.getParam("builddir", "b")
	config.sourceDir = init.getParam("sourcesdir", "s")
	config.deployType = init.getParam("deploytype", "d")
	config.sshOptions = init.getParam("ssh_options", "")
	config.kernSrcInstallDir = init.getParam("src_inst_dir", "")
	config.crosBoard = init.getParam("board", "")
	config.workdir = init.getParam("workdir", "w")
	config.crosPath = init.getParam("cros_sdk", "c")
	config.deployParams = init.getParam("target", "")
	config.ignoreCross = init.getParam("ignore_cros", "") == "1"

	if config.deployType == "" {
		config.deployType = "ssh"
	}

	return config
}

func (init *Init) isKernelSroucesDir(path string) bool {
	var files = []string{"Kbuild", "Kconfig", "Makefile"}
	for _, file := range files {
		if !fileExists(path + file) {
			return false
		}
	}

	return true
}

func (init *Init) isKernelBuildDir(path string) bool {
	var files = []string{"vmlinux", "System.map", "Makefile", ".config", "include/generated/uapi/linux/version.h"}
	for _, file := range files {
		if !fileExists(path + file) {
			return false
		}
	}

	return true
}

func (init *Init) isKlpEnabled(linuxHeadersDir string) bool {
	config, _ := os.ReadFile(linuxHeadersDir + ".config")
	if !bytes.Contains(config, []byte("CONFIG_LIVEPATCH=y")) {
		return false
	}
	systemMap, _ := os.ReadFile(linuxHeadersDir + "System.map")
	return bytes.Contains(systemMap, []byte("klp_enable_patch"))
}

func (init *Init) isLLVMUsed(linuxHeadersDir string) bool {
	config, _ := os.ReadFile(linuxHeadersDir + ".config")
	return bytes.Contains(config, []byte("CONFIG_CC_IS_CLANG=y"))
}

func (init *Init) checkConfigForCros(config *Config) error {
	var baseDir = ""
	if config.crosPath != "" {
		baseDir = config.crosPath + "chroot/"
	}
	insideCros := fileExists("/etc/cros_chroot_version")
	if !insideCros {
		LOG_ERR(nil, "Build kernel for Chromebook outside of CrOS SDK is not supported yet")
		return errors.New("ERROR_INVALID_PARAMETERS")
	}

	if config.crosBoard == "" {
		LOG_ERR(nil, "Please specify the Chromebook board name using: %s --board=<BOARD_NAME> ... syntax", os.Args[0])
		return errors.New("ERROR_NO_BOARD_PARAM")
	}

	if !insideCros && !fileExists(config.crosPath) {
		LOG_ERR(nil, "Given cros_sdk path is invalid")
		return errors.New("ERROR_INVALID_PARAMETERS")
	}

	if !fileExists(baseDir + "/build/" + config.crosBoard) {
		LOG_ERR(nil, "Please setup the board using \"setup_board\" command")
		return errors.New("ERROR_BOARD_NOT_EXISTS")
	}

	if config.buildDir != "" {
		LOG_ERR(nil, "-b|--builddir parameter can not be used for Chromebook kernel")
		return errors.New("ERROR_INVALID_PARAMETERS")
	}

	kerndir, err := CrosKernelName(baseDir, *config)
	if err != nil {
		return err
	}

	config.buildDir = filepath.Join(baseDir, "/build/", config.crosBoard, "/var/cache/portage/sys-kernel", kerndir) + "/"

	if config.kernSrcInstallDir == "" {
		config.kernSrcInstallDir = filepath.Join(baseDir, "/build/", config.crosBoard, "/usr/src/"+kerndir+"-9999") + "/"
	}
	if !fileExists(config.kernSrcInstallDir) {
		LOG_ERR(nil, "Kernel must be build with: USE=\"livepatch kernel_sources\" emerge-%s chromeos-kernel-...", config.crosBoard)
		return errors.New("ERROR_INSUFFICIENT_BUILD_PARAMS")
	}

	if config.sourceDir == "" && !insideCros {
		srcDir, _ := os.Readlink(config.buildDir + "source")
		srcDir = strings.TrimPrefix(srcDir, "/mnt/host/source/")
		config.sourceDir = filepath.Join(config.crosPath, srcDir) + "/"
	}

	if config.workdir == "" {
		config.workdir = cacheDir + "/workdir_" + config.crosBoard + "/"
	}
	os.MkdirAll(config.workdir, 0755)

	if !fileExists(config.workdir + "testing_rsa") {
		var GCLIENT_ROOT = "~/chromiumos/"
		copyFile(GCLIENT_ROOT+"/src/third_party/chromiumos-overlay/chromeos-base/chromeos-ssh-testkeys/files/testing_rsa",
			config.workdir+"testing_rsa")
		os.Chmod(config.workdir+"testing_rsa", 0400)
	}

	if config.sshOptions == "" {
		config.sshOptions = " -o IdentityFile=" + config.workdir + "/testing_rsa -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o BatchMode=yes -q"
	}

	if config.deployParams != "" && !strings.Contains(config.deployParams, "@") {
		config.deployParams = "root@" + config.deployParams
	}
	config.deployType = "ssh"

	return nil
}

func (init *Init) checkConfig(config *Config) error {
	if !config.ignoreCross && fileExists("/etc/cros_chroot_version") ||
		(config.crosBoard != "" && config.crosPath != "") {
		err := init.checkConfigForCros(config)
		if err != nil {
			return err
		}
	}

	if config.buildDir == "" {
		LOG_ERR(os.ErrInvalid, "Please specify the kernel build directory using -b or --builddir parameter")
		return errors.New("ERROR_NO_BUILDDIR")
	}

	if config.workdir == "" {
		path, _ := os.Executable()
		hasher := crc32.NewIEEE()
		hasher.Write([]byte(path))
		sum := hex.EncodeToString(hasher.Sum(nil))
		config.workdir = cacheDir + "/workdir_" + sum + "/"
	}

	if config.sourceDir == "" {
		srcDir := config.buildDir + "source"
		if _, err := os.Readlink(srcDir); err == nil {
			config.sourceDir = srcDir + "/"
		} else {
			config.sourceDir = config.buildDir
		}
	} else if !init.isKernelSroucesDir(config.sourceDir) {
		LOG_ERR(nil, `Given source directory is not a valid kernel source directory: "%s"`, config.sourceDir)
		return errors.New("ERROR_INVALID_KERN_SRC_DIR")
	}

	LOG_DEBUG("Source dir: %s", config.sourceDir)
	LOG_DEBUG("Build dir: %s", config.buildDir)
	LOG_DEBUG("Workdir: %s", config.workdir)

	if err := os.MkdirAll(config.workdir, 0755); err != nil {
		LOG_ERR(err, "Failed to create directory %s", config.workdir)
		return err
	}

	if !init.isKernelBuildDir(config.buildDir) {
		LOG_ERR(nil, `Given directory is not a valid kernel build directory: "%s"`, config.buildDir)
		return errors.New("ERROR_INVALID_BUILDDIR")
	}

	if !init.isKlpEnabled(config.buildDir) {
		if config.crosBoard != "" {
			LOG_ERR(nil, `Your kernel must be build with: USE="livepatch kernel_sources" emerge-%s chromeos-kernel-...`, config.crosBoard)
			return errors.New("ERROR_INSUFFICIENT_BUILD_PARAMS")
		} else {
			LOG_ERR(nil, "Kernel livepatching is not enabled. Please enable CONFIG_LIVEPATCH flag and rebuild the kernel")
			return errors.New("ERROR_KLP_IS_NOT_ENABLED")
		}
	}

	if init.isLLVMUsed(config.buildDir) {
		config.useLLVM = "LLVM=1"
	}

	config.linuxHeadersDir = config.buildDir
	config.modulesDir = config.buildDir
	config.systemMap = config.buildDir + "System.map"

	return nil
}

func (init *Init) init() (Config, error) {
	cacheDir, _ = os.Getwd() // TODO:
	init.config = init.getConfig()
	debugPrintConfig(&init.config)
	err := init.checkConfig(&init.config)
	debugPrintConfig(&init.config)
	return init.config, err
}

func debugPrintConfig(config *Config) {
	LOG_DEBUG("-----------CONFIG-----------")
	if config.buildDir != "" {
		LOG_DEBUG("buildDir: %s", config.buildDir)
	}
	if config.crosBoard != "" {
		LOG_DEBUG("crosBoard: %s", config.crosBoard)
	}
	if config.crosPath != "" {
		LOG_DEBUG("crosPath: %s", config.crosPath)
	}
	if config.deployParams != "" {
		LOG_DEBUG("deployParams: %s", config.deployParams)
	}
	if config.deployType != "" {
		LOG_DEBUG("deployType: %s", config.deployType)
	}
	LOG_DEBUG("ignoreCross: %v", config.ignoreCross)
	LOG_DEBUG("isCros: %v", config.isCros)
	if config.kernSrcInstallDir != "" {
		LOG_DEBUG("kernSrcInstallDir: %s", config.kernSrcInstallDir)
	}
	if config.kernelVersion != 0 {
		LOG_DEBUG("kernelVersion: %d", config.kernelVersion)
	}
	if config.linuxHeadersDir != "" {
		LOG_DEBUG("linuxHeadersDir: %s", config.linuxHeadersDir)
	}
	if config.modulesDir != "" {
		LOG_DEBUG("modulesDir: %s", config.modulesDir)
	}
	if config.sourceDir != "" {
		LOG_DEBUG("sourceDir: %s", config.sourceDir)
	}
	if config.sshOptions != "" {
		LOG_DEBUG("sshOptions: %s", config.sshOptions)
	}
	if config.systemMap != "" {
		LOG_DEBUG("systemMap: %s", config.systemMap)
	}
	if config.useLLVM != "" {
		LOG_DEBUG("useLLVM: %s", config.useLLVM)
	}
	if config.workdir != "" {
		LOG_DEBUG("workdir: %s", config.workdir)
	}
	LOG_DEBUG("----------------------------")
}
