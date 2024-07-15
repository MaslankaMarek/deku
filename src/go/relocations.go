package main

/*
#include <stdlib.h>

int mklivepatch(const char *file, const char *objName, char *syms);

#cgo LDFLAGS: mklivepatch.o
*/
import "C"

import (
	"debug/elf"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
	"unsafe"
)

type relocation struct {
	Name    string
	SymType elf.SymType
	ObjPath string
	Index   uint16
}

func findObjWithSymbol(sym, srcFile, objPath string) (string, error) {
	// TODO: Consider checking type of the symbol
	LOG_DEBUG("Find object file for symbol: %s %s", sym, srcFile)
	if objPath == "vmlinux" {
		return "vmlinux", nil
	}

	re := regexp.MustCompile("(?m)^" + strings.ReplaceAll(sym, ".", "\\.") + "$")

	symObjPath := strings.TrimSuffix(objPath, ".ko")
	symObjPath = filepath.Join(config.workdir, SYMBOLS_DIR, symObjPath)
	data, _ := os.ReadFile(symObjPath)
	if re.FindString(string(data)) != "" {
		LOG_DEBUG("Found in the same module: %s", objPath)
		return objPath, nil
	}

	srcPath := filepath.Join(config.sourceDir, filepath.Dir(srcFile))
	modulesPath := filepath.Join(config.modulesDir, filepath.Dir(srcFile))
	for {
		files := readLines(filepath.Join(modulesPath, "modules.order"))
		if len(files) == 0 {
			break
		}

		for _, file := range files {
			path := filepath.Join(config.workdir, SYMBOLS_DIR, filepath.Dir(file))
			if !generateSymbols(file) {
				continue
			}

			var files, err = os.ReadDir(path)
			if err != nil {
				LOG_ERR(err, "Fail to list files in: %s", path)
				return "", err
			}
			for _, symbolsFile := range files {
				if symbolsFile.IsDir() {
					continue
				}
				data, err := os.ReadFile(filepath.Join(path, symbolsFile.Name()))
				if err != nil {
					LOG_ERR(err, "Fail to read file: %s", filepath.Join(path, symbolsFile.Name()))
					return "", err
				}
				if re.FindString(string(data)) != "" {
					res := filepath.Join(filepath.Dir(file), symbolsFile.Name()) + ".ko"
					LOG_DEBUG("Found in: %s", res)
					return res, nil
				}
			}
		}
		if fileExists(filepath.Join(srcPath, "Kconfig")) {
			break
		}
		srcPath = filepath.Dir(srcPath)
		modulesPath = filepath.Dir(modulesPath)
		if modulesPath+"/" == config.modulesDir {
			break
		}
	}
	systemMap, err := os.ReadFile(config.systemMap)
	if err != nil {
		LOG_ERR(err, "Fail to read System.map: %s", config.systemMap)
		return "", err
	}
	if re.FindString(string(systemMap)) != "" {
		LOG_DEBUG("Found in: vmlinux")
		return "vmlinux", nil
	}
	LOG_ERR(err, "Fail to find object file for symbol: %s %s", sym, srcFile)
	os.Exit(ERROR_CANT_FIND_SYMBOL)
	return "", errors.New("Symbol not found")
}

func getSymbolsToRelocate(module, extraSymvers string) []relocation {
	var syms []relocation
	ignoreSymbols := []string{"_printk"}
	undSymbols, _ := getUndefinedSymbols(module)

	for _, sym := range undSymbols {
		if strings.HasPrefix(sym.Name, DEKU_FUN_PREFIX) {
			continue
		}

		if slicesContains(ignoreSymbols, sym.Name) {
			continue
		}

		symvers, _ := os.ReadFile(config.linuxHeadersDir + "vmlinux.symvers")
		re := regexp.MustCompile("\\b" + sym.Name + "\\b")
		if re.FindString(string(symvers)) != "" {
			continue
		}

		symvers, _ = os.ReadFile(config.linuxHeadersDir + "Module.symvers")
		if re.FindString(string(symvers)) != "" {
			continue
		}

		if extraSymvers != "" {
			symvers, _ = os.ReadFile(config.linuxHeadersDir + extraSymvers)
			if re.FindString(string(symvers)) != "" {
				continue
			}
		}

		syms = append(syms, relocation{
			Name:    sym.Name,
			SymType: elf.ST_TYPE(sym.Info),
			Index:   0,
		})
	}
	LOG_DEBUG("Symbols to relocate: %s", syms)
	return syms
}

func isContainsSymbol(objFilePath, symName, symType string) bool {
	LOG_DEBUG("Check if %s contains symbol: [%s] %s", objFilePath, symType, symName)
	e, err := Open(objFilePath)
	if err != nil {
		return false
	}
	defer e.Close()

	for _, symbol := range e.Symbols {
		if symbol.Size == 0 {
			continue
		}
		if (elf.ST_TYPE(symbol.Info) == elf.STT_FUNC ||
			elf.ST_TYPE(symbol.Info) == elf.STT_OBJECT || symType == "") &&
			symbol.Name == symName {
			if symType == "f" &&
				elf.ST_TYPE(symbol.Info) == elf.STT_FUNC {
				return true
			} else if symType == "v" &&
				elf.ST_TYPE(symbol.Info) == elf.STT_OBJECT {
				return true
			} else if symType == "" {
				return true
			}
		}
	}
	return false
}

func findDekuModuleWithSymbol(symName, symType string) string {
	for _, module := range getDekuModules(false) {
		objFile := filepath.Join(config.workdir, module.Name, module.Name+".o")
		if isContainsSymbol(objFile, symName, symType) {
			return module.Name
		}
	}
	return ""
}

func adjustRelocations(module dekuModule, objPath string, modSymbols []string) error {
	var mklivepatchArgs = []string{}
	var relSyms = []string{}
	var missSymErr error = nil
	var dependenciesFile = filepath.Dir(module.KoFile) + "/" + DEPS
	var missSymFile = filepath.Dir(module.KoFile) + "/" + MISS_SYM
	os.Remove(dependenciesFile)
	os.Remove(missSymFile)
	toRelocate := getSymbolsToRelocate(module.KoFile, "")
	for _, symbol := range toRelocate {
		if slicesContains(modSymbols, symbol.Name) {
			continue
		}

		if symbol.Name == "_GLOBAL_OFFSET_TABLE_" {
			continue
		}

		// TODO: If modified file is built into vmlinux then relocation must be in vmlinux
		symObjPath, err := findObjWithSymbol(symbol.Name, module.SrcFile, objPath)
		if err != nil {
			LOG_ERR(err, "Can't find symbol: %s", symbol.Name)
			os.Exit(ERROR_CANT_FIND_SYMBOL)
		}
		symbol.ObjPath = symObjPath
		symType := ""
		if symbol.SymType == elf.STT_FUNC {
			symType = "f"
		} else if symbol.SymType == elf.STT_OBJECT {
			symType = "v"
		}
		index, err := findSymbolIndex(symbol.Name, symType, module.SrcFile,
			config.buildDir+symObjPath)
		if err != nil {
			if index != -ERROR_CANT_FIND_SYM_INDEX {
				return err
			}
			dekuModWithSym := findDekuModuleWithSymbol(symbol.Name, symType)
			if dekuModWithSym != "" {
				symObjPath = dekuModWithSym
				index = 0
				if err = appendToFile(dependenciesFile, dekuModWithSym+"\n"); err != nil {
					return err
				}
				LOG_DEBUG("%s depends on %s due to symbol required: %s", module.Name, dekuModWithSym,
					symbol.Name)
			} else {
				missSymErr = err
				if err = appendToFile(missSymFile, symbol.Name); err != nil {
					return err
				}
				continue
			}
		}

		relSym := fmt.Sprintf("%s.%s,%d", filenameNoExt(symObjPath), symbol.Name, index)
		mklivepatchArgs = append(mklivepatchArgs, "-r", relSym)
		relSyms = append(relSyms, relSym)
	}

	if missSymErr != nil {
		return missSymErr
	}

	for _, modSym := range modSymbols {
		mklivepatchArgs = append(mklivepatchArgs, "-s",
			fmt.Sprintf("%s.%s", filenameNoExt(objPath), modSym))
	}

	var err error

	if USE_EXTERNAL_EXECUTABLE {
		mklivepatchArgs = append(mklivepatchArgs, "-V", module.KoFile)

		cmd := exec.Command("./mklivepatch", mklivepatchArgs...)
		cmd.Stdout = os.Stdout
		cmd.Stderr = os.Stderr
		err = cmd.Run()
	} else {
		file := C.CString(module.KoFile)
		obj := C.CString(filenameNoExt(objPath))
		syms := C.CString(strings.Join(relSyms, "|"))
		errCode := C.mklivepatch(file, obj, syms)
		if errCode != 0 {
			err = errors.New("Making livepatch error: " + string(errCode))
		}
		C.free(unsafe.Pointer(obj))
		C.free(unsafe.Pointer(file))
	}

	if err != nil {
		LOG_ERR(err, "Failed to mklivepatch for %s", module.SrcFile)
		return err
	}
	return nil
}
