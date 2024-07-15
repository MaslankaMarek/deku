package main

import (
	"embed"
	"encoding/json"
	"fmt"
	"os"
	"strconv"
	"strings"
)

//go:embed resources
var resources embed.FS

var USE_EXTERNAL_EXECUTABLE = false

var MinKernelVersion = "5.4"

func checkWorkdir() {
	workdirCfgFile, err := os.ReadFile(config.workdir + "config")
	workdirCfg := map[string]string{}

	if err != nil {
		LOG_DEBUG("%s", err)
		goto sync
	}

	json.Unmarshal(workdirCfgFile, &workdirCfg)

	if workdirCfg[KERNEL_VERSION] != getKernelVersion() {
		goto sync
	}

	if config.kernSrcInstallDir != "" {
		kernStat, _ := os.Stat(config.kernSrcInstallDir)
		cfgStat, _ := os.Stat(config.workdir + "config")
		if !kernStat.ModTime().Equal(cfgStat.ModTime()) {
			goto sync
		}
	}

	if workdirCfg[DEKU_HASH] != generateDEKUHash() {
		goto sync
	}

	return
sync:
	synchronize()
}

func prepareConfig() {
	var init Init
	cfg, err := init.init()
	if err != nil {
		LOG_ERR(err, "")
		os.Exit(1)
	}
	config = cfg

	ver := strings.Split(getKernelReleaseVersion(), ".")
	minVer := strings.Split(MinKernelVersion, ".")
	verMajor, _ := strconv.ParseInt(ver[0], 10, 0)
	verMinor, _ := strconv.ParseInt(ver[1], 10, 0)
	minVerMajor, _ := strconv.ParseInt(minVer[0], 10, 0)
	minVerMinor, _ := strconv.ParseInt(minVer[1], 10, 0)

	config.kernelVersion = versionNum(uint64(verMajor), uint64(verMinor), 0)

	if config.kernelVersion <
		versionNum(uint64(minVerMajor), uint64(minVerMinor), 0) {
		LOG_WARN("Kernel version: %s is not supported\nMinimum supported kernel version: %s",
			getKernelReleaseVersion(), MinKernelVersion)
		os.Exit(1)
	}

	checkWorkdir()
}

func main() {
	if len(os.Args) < 2 {
		LOG_ERR(nil, "Not enought parameters")
		os.Exit(1)
	}

	for _, arg := range os.Args {
		if arg == "-v" {
			LOG_LEVEL = 1
		}
	}

	if os.Args[1] == "filenameNoExt" {
		text := filenameNoExt(os.Args[2])
		fmt.Print(text)
		return
	} else if os.Args[1] == "generateModuleName" {
		name := generateModuleName(os.Args[2])
		fmt.Print(name)
		return
	} else if os.Args[1] == "isTraceable" {
		traceable, _ := checkIsTraceable(os.Args[2], os.Args[3])
		if traceable {
			os.Exit(0)
		} else {
			os.Exit(1)
		}
	} else if os.Args[1] == "noTraceable" {
		traceable, _ := checkIsTraceable(os.Args[2], os.Args[3])
		if traceable {
			os.Exit(0)
		} else {
			os.Exit(1)
		}
	}

	prepareConfig()

	if config.crosPath != "" {
		spawnInCros()
		return
	}

	action := os.Args[len(os.Args)-1]
	if action == "build" {
		build()
	} else if action == "deploy" {
		deploy()
	} else if action == "sync" {
		synchronize()
	} else {
		LOG_ERR(nil, "Invalid command: %s", action)
		os.Exit(1)
	}
}
