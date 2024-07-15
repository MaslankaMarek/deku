package main

import (
	"sort"
	"strings"
)

func checkKernels() (bool, error) {
	kernelRelease, err := getRemoteKernelRelease()
	if err != nil {
		LOG_ERR(err, "Fail to fetch the kernel release information from the device")
		return false, err
	}
	kernelVersion, err := getRemoteKernelVersion()
	if err != nil {
		LOG_ERR(err, "Fail to fetch the kernel version from the device")
		return false, err
	}
	localKernelRelease := getKernelReleaseVersion()
	localKernelVersion := getKernelVersion()

	if strings.Contains(kernelRelease, localKernelRelease) &&
		strings.Contains(kernelVersion, localKernelVersion) {
		return true, nil
	}
	LOG_ERR(nil, "The kernel on the device is outdated!")
	LOG_INFO("Kernel on the device: %s %s", kernelRelease, kernelVersion)
	LOG_INFO("Local built kernel:   %s %s", localKernelRelease, localKernelVersion)
	return false, nil
}

func sortModules(modules []dekuModule, depFirst bool) {
	sort.SliceStable(modules, func(i, j int) bool {
		if len(modules[i].Dependencies) == 0 {
			return depFirst
		}
		if len(modules[j].Dependencies) == 0 {
			return !depFirst
		}
		if depFirst {
			return slicesContains(modules[j].Dependencies, modules[i].Name)
		} else {
			return !slicesContains(modules[j].Dependencies, modules[i].Name)

		}
	})
}

func deploy() {
	if config.deployType == "" || config.deployParams == "" {
		LOG_ERR(nil, "Please specify SSH connection parameters to the target device using: --target=<user@host[:port]> parameter")
		return
	}

	if config.deployType != "ssh" {
		LOG_ERR(nil, "Unknown deploy type '%s'", config.deployType)
		return
	}

	startDaemon()
	isKernelValid, err := checkKernels()
	if err != nil {
		return
	}
	if !isKernelValid {
		LOG_WARN("Please install the current built kernel on the device")
		return
	}

	build()

	modulesToLoad := []dekuModule{}
	modulesToUnload := []dekuModule{}
	modulesOnDevice, _ := getLoadedDEKUModules()
	LOG_DEBUG("Modules on the device %s", modulesOnDevice)
	for _, remoteModule := range modulesOnDevice {
		arr := strings.Split(remoteModule, " ")
		moduleName := arr[0]
		if !fileExists(config.workdir + moduleName + "/" + moduleName + ".ko") {
			module := dekuModule{Name: moduleName, SrcFile: arr[1], Dependencies: strings.Split(arr[3], ",")}
			modulesToUnload = append(modulesToUnload, module)
			LOG_INFO("Revert changes on the device for " + arr[1])
			continue
		}
	}

	localModules := getDekuModules(true)
	sortModules(localModules, true)
	sortModules(modulesToUnload, false)

	LOG_DEBUG("Local DEKU modules: %s", localModules)
	for _, localModule := range localModules {
		moduleIsLoaded := false
		for _, remoteModule := range modulesOnDevice {
			arr := strings.Split(remoteModule, " ")
			moduleName := arr[0]
			if moduleName != localModule.Name {
				continue
			}
			moduleId := arr[2][:8]
			idFile := config.workdir + moduleName + "/id"
			if fileExists(idFile) {
				if readLines(idFile)[0] == moduleId {
					moduleIsLoaded = true
				}
			}
			break
		}
		if moduleIsLoaded {
			continue
		}
		modulesToLoad = append(modulesToLoad, localModule)
	}
	if len(modulesToLoad) == 0 && len(modulesToUnload) == 0 {
		LOG_INFO("No changes need to be made to the device")
		return
	}

	uploadAndLoadModules(modulesToLoad, modulesToUnload)
}
