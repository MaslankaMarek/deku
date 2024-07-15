package main

import (
	"bytes"
	"crypto/md5"
	"debug/elf"
	"encoding/hex"
	"errors"
	"fmt"
	"hash/crc32"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
)

var config Config

// Log level filter
var LOG_LEVEL = 2 // 1 - debug, 2 - info, 3 - warning, 4 - error

func LOG_FATAL(err error, format string, args ...any) {
	LOG_ERR(err, format, args...)
	os.Exit(1)
}

func LOG_ERR(err error, format string, args ...any) {
	if LOG_LEVEL > 4 {
		return
	}
	if err != nil {
		fmt.Fprintf(os.Stderr, format+": ", args...)
		fmt.Fprintf(os.Stderr, "%s\n", err.Error())
	} else {
		fmt.Fprintf(os.Stderr, format+"\n", args...)
	}
}

func LOG_WARN(format string, args ...any) {
	if LOG_LEVEL > 3 {
		return
	}
	fmt.Fprintf(os.Stdout, format+"\n", args...)
}

func LOG_INFO(format string, args ...any) {
	if LOG_LEVEL > 2 {
		return
	}
	fmt.Fprintf(os.Stdout, format+"\n", args...)
}

func LOG_DEBUG(format string, args ...any) {
	if LOG_LEVEL > 1 {
		return
	}
	fmt.Fprintf(os.Stdout, format+"\n", args...)
}

func slicesContains(sl []string, name string) bool {
	for _, value := range sl {
		if value == name {
			return true
		}
	}
	return false
}

func removeDuplicate(sl []string) []string {
	list := []string{}
	for _, v := range sl {
		if !slicesContains(list, v) {
			list = append(list, v)
		}
	}
	return list
}

func fileExists(file string) bool {
	if _, err := os.Stat(file); err != nil {
		return !os.IsNotExist(err)
	}
	return true
}

func appendToFile(file, text string) error {
	f, err := os.OpenFile(file, os.O_APPEND|os.O_WRONLY|os.O_CREATE, 0600)
	if err != nil {
		return err
	}
	defer f.Close()

	if _, err = f.WriteString(text); err != nil {
		return err
	}
	return nil
}

func filenameNoExt(path string) string {
	base := filepath.Base(path)
	ext := filepath.Ext(base)
	return base[:len(base)-len(ext)]
}

func modifiedFiles() []string {
	ignoredFilesH := []string{"arch/x86/realmode/rm/pasyms.h",
		"arch/x86/boot/voffset.h",
		"arch/x86/boot/cpustr.h",
		"arch/x86/boot/zoffset.h",
		"init/utsversion-tmp.h"}
	ignoredFilesC := []string{"arch/x86/entry/vdso/vdso-image-32.c",
		"arch/x86/entry/vdso/vdso-image-64.c"}

	file, err := os.Stat(config.buildDir + ".config")
	if err != nil {
		LOG_ERR(err, "Can't find .config file")
		return nil
	}
	configTime := file.ModTime()

	files := []string{}
	err = filepath.Walk(config.sourceDir, func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}

		if info.IsDir() {
			return nil
		}

		path = path[len(config.sourceDir):]

		if configTime.After(info.ModTime()) {
			return nil
		}

		if strings.HasPrefix(path, "include/generated/") ||
			strings.HasPrefix(path, "scripts/") ||
			strings.HasPrefix(path, ".git/") ||
			strings.HasPrefix(path, "Documentation/") {
			return nil
		}

		if config.kernSrcInstallDir != "" {
			fileStat, _ := os.Stat(config.sourceDir + path)
			if matched, _ := filepath.Match("*.c", fileStat.Name()); !matched {
				if matched, _ := filepath.Match("*.h", fileStat.Name()); !matched {
					return nil
				}
			}

			originFileStat, _ := os.Stat(config.kernSrcInstallDir + path)
			if originFileStat != nil && fileStat.ModTime().After(originFileStat.ModTime()) {
				f1, _ := os.ReadFile(config.sourceDir + path)
				f2, _ := os.ReadFile(config.kernSrcInstallDir + path)
				if !bytes.Equal(f1, f2) {
					files = append(files, path)
				}
			}
		} else {
			if matched, _ := filepath.Match("*.c", info.Name()); matched {
				if matched, _ := filepath.Match("*.mod.c", info.Name()); !matched {
					if !slicesContains(ignoredFilesC, path) {
						files = append(files, path)
					}
				}
			}

			if matched, _ := filepath.Match("*.h", info.Name()); matched {
				if !slicesContains(ignoredFilesH, path) {
					files = append(files, path)
				}
			}
		}

		return nil
	})

	if err != nil {
		fmt.Println(err)
		return nil
	}

	LOG_DEBUG("Modified files: %s", files)
	return files
}

func generateSymbols(koFile string) bool {
	path := filepath.Dir(koFile)
	outDir := filepath.Join(config.workdir, SYMBOLS_DIR, path)
	outFile := filepath.Join(outDir, filenameNoExt(filepath.Base(koFile)))

	if fileExists(outFile) {
		return true
	}

	LOG_DEBUG("Generate symbols for: %s", koFile)

	// Check if the module is enabled in the kernel configuration.
	modules, err := os.ReadFile(filepath.Join(config.modulesDir, path, "modules.order"))
	if err != nil {
		LOG_DEBUG("Can't find modules.order file")
		return false
	}
	if !bytes.Contains(modules, []byte(koFile)) {
		LOG_DEBUG(fmt.Sprintf("The module %s file is not enabled in the current kernel configuration", koFile))
		return false
	}

	if err := os.MkdirAll(outDir, 0755); err != nil {
		LOG_DEBUG("Can't create symbol dir: %s\n%s", outDir, err)
		return false
	}

	path = filepath.Join(config.modulesDir, koFile)
	readelfCmd := exec.Command("readelf",
		"--symbols",
		"--wide",
		path)

	output, err := readelfCmd.Output()
	if err != nil {
		LOG_DEBUG("Fail to read symbols for: %s\n%s", path, err)
		return false
	}

	lines := strings.Split(string(output), "\n")
	var symbols []string

	for _, line := range lines {
		fields := strings.Fields(line)
		if len(fields) >= 8 && (fields[3] == "FUNC" || fields[3] == "OBJECT") {
			symbols = append(symbols, fields[7])
		}
	}

	result := strings.Join(symbols, "\n")
	err = os.WriteFile(outFile, []byte(result), 0644)
	if err != nil {
		LOG_DEBUG("Fail to write symbols to file: %s\n%s", outFile, err)
		return false
	}

	return true
}

type embedObjSymbols struct {
	symbols  []elf.Symbol
	index    int
	offset   uint64
	hitCount int
}

func findSymbolIndex(symName, symType, srcFile, objFilePath string) (int, error) {
	LOG_DEBUG("Finding index for symbol: %s [%s] from source file: %s in: %s", symName, symType, srcFile, objFilePath)
	e, err := Open(objFilePath)
	if err != nil {
		return -1, err
	}
	defer e.Close()

	symbolsInObj := []embedObjSymbols{}
	var fileSyms embedObjSymbols
	var symOffset uint64 = 0
	symCount := 0
	index := 0
	hitCount := 0
	srcFileName := filepath.Base(srcFile)
	currentName := ""
	offsets := []uint64{}
	for _, symbol := range e.Symbols {
		if elf.ST_TYPE(symbol.Info) == elf.STT_FILE {
			currentName = symbol.Name
			if currentName == srcFileName {
				fileSyms.symbols = []elf.Symbol{}
			}
			continue
		} else if (elf.ST_TYPE(symbol.Info) == elf.STT_FUNC ||
			elf.ST_TYPE(symbol.Info) == elf.STT_OBJECT || symType == "") &&
			symbol.Name == symName {
			symCount++
			if symType == "f" &&
				elf.ST_TYPE(symbol.Info) != elf.STT_FUNC {
				continue
			}
			if symType == "v" &&
				elf.ST_TYPE(symbol.Info) != elf.STT_OBJECT {
				continue
			}
			index++
			offsets = append(offsets, symbol.Value)
			if currentName == srcFileName {
				fileSyms.index = index
				fileSyms.offset = symbol.Value
				symbolsInObj = append(symbolsInObj, fileSyms)
			}
		}
		if currentName == srcFileName {
			fileSyms.symbols = append(fileSyms.symbols, symbol)
		}
	}

	if symCount == 0 {
		return -ERROR_CANT_FIND_SYM_INDEX, errors.New(fmt.Sprintln("can't find any symbol index for", symName))
	} else if symCount == 1 {
		index = 0
		goto out
	} else if len(symbolsInObj) == 1 {
		symOffset = symbolsInObj[0].offset
	} else {
		e, err = Open(config.buildDir + strings.TrimSuffix(srcFile, "c") + "o")
		if err != nil {
			return -1, err
		}
		defer e.Close()

		LOG_DEBUG("Found %d objects file with symbol [%s] %s", len(symbolsInObj), symType, symName)

		for k, symbols := range symbolsInObj {
			for _, sym := range e.Symbols {
				for _, s := range symbols.symbols {
					if sym.Size == s.Size && sym.Name == s.Name &&
						sym.Info == s.Info {
						symbolsInObj[k].hitCount++
						break
					}
				}
			}
			cnt := symbolsInObj[k].hitCount
			LOG_DEBUG("Hit count for %d: %d", k, cnt)
			if cnt > hitCount {
				hitCount = cnt
				symOffset = symbolsInObj[k].offset
			}
		}
		if symOffset == 0 {
			return -1, errors.New("can't find properly symbol index because there are multiple symbols with the same name")
		}
	}

	index = 1
	for _, offset := range offsets {
		if offset < symOffset {
			index++
		}
	}

out:
	LOG_DEBUG("Found at index %d", index)
	return index, nil
}

func getKernelInformation(info string) string {
	var result = ""
	re := regexp.MustCompile(`.*` + info + ` "(.+)"\n.*`)
	_ = filepath.Walk(config.buildDir+"/include/generated/", func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}

		if info.IsDir() {
			return nil
		}
		file, _ := os.ReadFile(path)
		match := re.FindStringSubmatch(string(file))
		if len(match) > 1 {
			result = match[1]
			return io.EOF
		}
		return nil
	})
	return result
}

func getKernelVersion() string {
	return getKernelInformation("UTS_VERSION")
}

func getKernelReleaseVersion() string {
	return getKernelInformation("UTS_RELEASE")
}

func generateModuleName(file string) string {
	hasher := crc32.NewIEEE()
	hasher.Write([]byte(file))
	sum := hex.EncodeToString(hasher.Sum(nil))
	name := filenameNoExt(file)
	name = strings.Replace(name, "-", "_", -1)

	return "deku_" + sum + "_" + name
}

func getDekuModules(onlyValidModules bool) []dekuModule {
	modules := []dekuModule{}
	files, err := os.ReadDir(config.workdir)
	if err != nil {
		return modules
	}

	for _, dekuMod := range files {
		if !dekuMod.IsDir() || !strings.HasPrefix(dekuMod.Name(), "deku_") {
			continue
		}
		src, err := os.ReadFile(
			filepath.Join(config.workdir, dekuMod.Name(), FILE_SRC_PATH))
		if err != nil {
			continue
		}
		koFile := filepath.Join(
			config.workdir, dekuMod.Name(), dekuMod.Name()+".ko")
		idFile := filepath.Join(config.workdir, dekuMod.Name(), "id")
		if onlyValidModules && (!fileExists(koFile) || !fileExists(idFile)) {
			continue
		}
		if !fileExists(koFile) {
			koFile = ""
		}

		module := dekuModule{
			SrcFile:      string(src),
			Name:         dekuMod.Name(),
			KoFile:       koFile,
			Dependencies: readLines(filepath.Join(config.workdir, dekuMod.Name(), DEPS)),
		}
		modules = append(modules, module)
	}
	return modules
}

func generateDEKUHash() string {
	dekuPath, err := os.Executable()
	if err != nil {
		panic(err)
	}
	data, err := os.ReadFile(dekuPath)
	if err != nil {
		LOG_ERR(err, "Can't read file")
	}
	hash := md5.Sum([]byte(fmt.Sprintf("%x", data)))
	return hex.EncodeToString(hash[:])
}

func versionNum(major, minor, patch uint64) uint64 {
	maxPatch := uint64(99999)
	maxMinor := uint64(9999)
	return major*(maxMinor+1)*(maxPatch+1) + minor*(maxPatch+1) + patch
}
