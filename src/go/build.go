package main

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
)

func removeOldModules(files []string) {
	onlyValidModules := config.kernSrcInstallDir == ""
	for _, module := range getDekuModules(onlyValidModules) {
		if !slicesContains(files, module.SrcFile) {
			moduleDir := config.workdir + module.Name
			if config.kernSrcInstallDir == "" {
				// remove all but id file
				files, _ := os.ReadDir(moduleDir)
				for _, file := range files {
					if file.Name() != "id" {
						os.Remove(filepath.Join(moduleDir, file.Name()))
					}
				}
			} else {
				os.RemoveAll(moduleDir)
			}
		}
	}
}

func build() []dekuModule {
	var cros Cros
	files := modifiedFiles()

	removeOldModules(files)

	if len(files) == 0 {
		LOG_INFO("No change detected in the source code")
		return []dekuModule{}
	}

	for _, file := range files {
		if filepath.Ext(file) != ".c" {
			LOG_WARN("Detected changes in %s. Only changes to '.c' files are supported.", file)
			if KERN_SRC_INSTALL_DIR != "" {
				LOG_WARN("Undo changes in %s and try again.", file)
				os.Exit(ERROR_UNSUPPORTED_CHANGES)
			}
			LOG_WARN("Rebuild the kernel to suppress this warning.")
		}
	}

	if config.crosBoard != "" && !config.ignoreCross {
		cros.preBuild()
		defer cros.postBuild()
	}

	modules := make([]dekuModule, len(files))
	wg := sync.WaitGroup{}
	for i, file := range files {
		wg.Add(1)
		go func(i int, file string) {
			defer wg.Done()
			module, err := generateModule(file)
			modules[i] = module
			if err != nil {
				missSym := filepath.Dir(module.KoFile) + "/" + MISS_SYM
				if fileExists(missSym) {
					return
				}
				LOG_ERR(err, "Failed to process %s", file)
				os.Exit(errorStrToCode(err))
			}
		}(i, file)
	}
	wg.Wait()

	// Try 2nd pass if some module missing symbols from other deku modules
	for i, module := range modules {
		if !isModuleValid(module) {
			moduleDir := filepath.Dir(module.KoFile)
			if fileExists(moduleDir + "/" + MISS_SYM) {
				os.Remove(moduleDir + "/" + "id")
				mod, err := generateModule(module.SrcFile)
				if err != nil {
					LOG_ERR(err, "Failed to process %s", module.SrcFile)
					os.Exit(errorStrToCode(err))
				}
				modules[i] = mod
			}
		}
	}

	result := []dekuModule{}
	for _, module := range modules {
		if !isModuleValid(module) {
			continue
		}

		// Add note to module with module name, path, id and dependencies
		moduleDir := filepath.Join(config.workdir, module.Name)
		moduleId, _ := generateModuleId(module.SrcFile)
		deps := strings.Join(readLines(filepath.Join(moduleDir, DEPS)), ",")
		notefile := filepath.Join(moduleDir, NOTE_FILE)
		data := []byte(module.Name + " " + module.SrcFile + " " + moduleId +
			" " + deps)
		os.WriteFile(notefile, data, 0644)
		cmd := exec.Command("objcopy", "--add-section", ".note.deku="+notefile,
			"--set-section-flags", ".note.deku=alloc,readonly", module.KoFile)
		cmd.Stderr = os.Stderr
		err := cmd.Run()
		if err != nil {
			LOG_ERR(err, "Failed to add note information to module")
			continue
		}

		result = append(result, module)
	}

	if len(result) == 0 {
		LOG_INFO("No valid changes detected since last run")
	}

	return result
}
