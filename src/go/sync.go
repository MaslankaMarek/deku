package main

import (
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"
)

func regenerateSymbols() {
	files := []string{}
	symbolsPath := config.workdir + SYMBOLS_DIR + "/"
	symbolsPath = strings.TrimPrefix(symbolsPath, "./")
	_ = filepath.Walk(symbolsPath, func(file string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}

		if info.IsDir() {
			return nil
		}
		files = append(files, file)
		return nil
	})

	os.RemoveAll(symbolsPath)
	os.Mkdir(symbolsPath, 0755)
	for _, file := range files {
		koFile := strings.TrimPrefix(file, symbolsPath) + ".ko"
		generateSymbols(koFile)
	}
}

func synchronize() {
	LOG_INFO("Synchronize...")

	modules, _ := filepath.Glob(config.workdir + "deku_*")
	for _, module := range modules {
		LOG_DEBUG("Remove %s", module)
		err := os.RemoveAll(module)
		if err != nil {
			LOG_ERR(err, "Cant remove %s", module)
		}
	}

	workdirCfg := make(map[string]string)
	workdirCfg[KERNEL_VERSION] = getKernelVersion()
	workdirCfg[DEKU_HASH] = generateDEKUHash()
	jsonStr, _ := json.Marshal(workdirCfg)
	os.WriteFile(config.workdir+"config", []byte(jsonStr), 0644)

	regenerateSymbols()

	if config.kernSrcInstallDir == "" {
		cmd := exec.Command("git", "--work-tree="+config.sourceDir, "--git-dir="+config.workdir+".git", "add", config.sourceDir+"*")
		cmd.Run()
	} else {
		stat, _ := os.Stat(config.kernSrcInstallDir)
		os.Chtimes(config.workdir+"config", time.Time{}, stat.ModTime())
	}
}
