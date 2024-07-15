package main

import (
	_ "embed"
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

type dekuModule struct {
	SrcFile      string
	Name         string
	KoFile       string
	Dependencies []string
}

func isModuleValid(module dekuModule) bool {
	return len(module.Name) > 0
}

func invalidateModule(module dekuModule) dekuModule {
	module.Name = ""
	return module
}

func generateModuleId(fileName string) (string, error) {

	file, err := os.Open(config.sourceDir + fileName)
	if err != nil {
		LOG_ERR(err, "Can't read %s file", config.sourceDir+fileName)
		return "", err
	}
	defer file.Close()

	hasher := crc32.NewIEEE()
	if _, err := io.Copy(hasher, file); err != nil {
		return "", err
	}
	sum := hex.EncodeToString(hasher.Sum(nil))

	return sum, nil
}

func getFileDiff(file string) ([]byte, error) {
	var cmd *exec.Cmd
	if config.kernSrcInstallDir != "" {
		cmd = exec.Command("diff",
			"--unified",
			fmt.Sprintf("%s/%s", config.kernSrcInstallDir, file),
			"--label", fmt.Sprintf("%s/%s", config.kernSrcInstallDir, file),
			fmt.Sprintf("%s/%s", config.sourceDir, file),
			"--label", fmt.Sprintf("%s/%s", config.sourceDir, file))
	} else {
		cmd = exec.Command("git",
			"-C", config.workdir,
			"diff",
			"--function-context",
			"--", file)
	}
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	return cmd.Output()
}

func generateLivepatchMakefile(makefile string, file string, module string) {
	outFile := filenameNoExt(module)

	f, err := os.Create(makefile)
	if err != nil {
		LOG_ERR(err, "Can't create Makefile")
	}
	defer f.Close()

	fmt.Fprintf(f, "KBUILD_MODPOST_WARN = 1\n")
	fmt.Fprintf(f, "KBUILD_CFLAGS += -ffunction-sections -fdata-sections\n") // FIXME: Remove

	fmt.Fprintf(f, "obj-m += %s.o\n", outFile)
	fmt.Fprintf(f, "%s-objs := livepatch.o patch.o\n", outFile)
	fmt.Fprintf(f, "all:\n")
	fmt.Fprintf(f, "	make -C %s M=\"%s%s\" modules\n", config.linuxHeadersDir, config.workdir, outFile)
	fmt.Fprintf(f, "clean:\n")
	fmt.Fprintf(f, "	make -C %s M=\"%s%s\" clean\n", config.linuxHeadersDir, config.workdir, outFile)

	os.Create(filepath.Dir(makefile) + "/.patch.o.cmd")
}

func generateDiffObject(moduleDir string, file string) ([]string, error) {
	fileName := filenameNoExt(file)
	oFile := moduleDir + "/" + fileName + ".o"
	originObjFile := config.buildDir + file[:len(file)-1] + "o"
	var extractSyms []string
	var modSyms []string

	out, err := showDiff(originObjFile, oFile)
	if err != nil {
		LOG_ERR(errors.New(string(out)), "Can't find modified functions for %s", file)
		return nil, err
	}

	LOG_DEBUG("Modified symbols:\n%s", string(out))

	tmpModFun := regexp.MustCompile(`\bModified function: (.+)\n`).FindAllStringSubmatch(string(out), -1)
	newFun := regexp.MustCompile(`\bNew function: (.+)\n`).FindAllStringSubmatch(string(out), -1)
	newVar := regexp.MustCompile(`\bNew variable: (.+)\n`).FindAllStringSubmatch(string(out), -1)

	for _, f := range tmpModFun {
		fun := f[1]
		if checkIfIsInitOrExit(oFile, fun) {
			continue
		}
		traceable, callers := checkIsTraceable(originObjFile, fun)
		if !traceable && len(callers) == 0 {
			LOG_ERR(nil, "Can't apply changes to the '%s' because the '%s' function is forbidden to modify.", file, fun)
			return nil, errors.New("ERROR_FORBIDDEN_MODIFY")
		}

		if traceable {
			modSyms = append(modSyms, fun)
		} else {
			for _, sym := range callers {
				modSyms = append(modSyms, sym)
				extractSyms = append(extractSyms, sym)
			}
		}
		extractSyms = append(extractSyms, fun)
	}

	if len(extractSyms) == 0 && len(newFun) == 0 && len(newVar) == 0 {
		return extractSyms, nil
	}

	for _, fun := range newFun {
		extractSyms = append(extractSyms, fun[1])
	}

	for _, variable := range newVar {
		extractSyms = append(extractSyms, variable[1])
	}

	extractSymbols(oFile, moduleDir+"/patch.o", removeDuplicate(extractSyms))
	if err != nil {
		LOG_ERR(errors.New(string(out)), "Failed to extract modified symbols for %s", file)
		// return errors.New("ERROR_EXTRACT_SYMBOLS")
		return nil, err
	}

	return removeDuplicate(modSyms), nil
}

func findObjectFile(srcFile string) (string, error) {
	objFile := srcFile[:len(srcFile)-len(filepath.Ext(srcFile))] + ".o"
	dir := filepath.Dir(srcFile)
	for {
		builtInACmdPath := filepath.Join(config.buildDir, dir, ".built-in.a.cmd")
		if _, err := os.Stat(builtInACmdPath); err == nil {
			content, _ := os.ReadFile(builtInACmdPath)
			baseName := filepath.Base(objFile)
			if strings.Contains(string(content), objFile) ||
				strings.Contains(string(content), " "+baseName+" ") {
				return "vmlinux", nil
			}
		}

		koFiles, err := filepath.Glob(filepath.Join(config.modulesDir, dir, "*.ko"))
		if err != nil {
			return "", err
		}

		if len(koFiles) > 0 {
			for _, koFile := range koFiles {
				koFile := koFile[len(config.modulesDir):]
				path := filepath.Dir(koFile)
				content, err := os.ReadFile(filepath.Join(config.modulesDir, path, "modules.order"))
				if err != nil {
					continue
				}
				if !strings.Contains(string(content), koFile) {
					continue
				}

				fileName := filenameNoExt(koFile)
				content, err = os.ReadFile(filepath.Join(config.modulesDir, path, fileName+".mod"))
				if err == nil && strings.Contains(string(content), objFile) {
					return koFile, nil
				}
			}
		}

		dir = filepath.Dir(dir)
		if dir == "/" || dir == "." {
			return "", errors.New("ERROR_CANT_FIND_OBJ")
		}
	}
}

func generateLivepatchSource(moduleDir, file string, modFuncs []string) error {
	outFilePath := filepath.Join(moduleDir, "livepatch.c")
	klpFunc := ""
	prototypes := ""

	objPath, err := findObjectFile(file)
	if err != nil {
		return err
	}

	objName := filenameNoExt(objPath)

	// Write the object file path to a file.
	err = os.WriteFile(filepath.Join(moduleDir, FILE_OBJECT_PATH), []byte(objPath), 0644)
	if err != nil {
		return err
	}

	os.Remove(outFilePath)
	outFile, err := os.OpenFile(outFilePath, os.O_APPEND|os.O_WRONLY|os.O_CREATE, 0644)
	if err != nil {
		panic(err)
	}
	defer outFile.Close()

	for _, symbol := range modFuncs {
		plainSymbol := strings.ReplaceAll(symbol, ".", "_")
		sympos, err := findSymbolIndex(symbol, "f", file, config.buildDir+objPath)
		if err != nil {
			return err
		}

		// Fill list of a klp_func struct.
		klpFunc += `	{
			  .old_name = "` + symbol + `",
			  .new_func = ` + DEKU_FUN_PREFIX + plainSymbol + `,
			  .old_sympos = ` + fmt.Sprint(sympos) + `
		  },`

		prototypes += `	void ` + DEKU_FUN_PREFIX + plainSymbol + `(void);`
	}

	klpObjName := "NULL"
	if objName != "vmlinux" {
		klpObjName = "\"" + objName + "\""
	}

	// Add to module necessary headers.
	_, err = outFile.WriteString(`
  #include <linux/kernel.h>
  #include <linux/module.h>
  #include <linux/livepatch.h>
  #include <linux/version.h>
  `)
	if err != nil {
		return err
	}

	// Add livepatching code.
	_, err = outFile.WriteString(fmt.Sprintf(`
  %s

  static struct klp_func deku_funcs[] = {
  %s { }
  };

  static struct klp_object deku_objs[] = {
	  {
		  .name = %s,
		  .funcs = deku_funcs,
	  }, { }
  };

  static struct klp_patch deku_patch = {
	  .mod = THIS_MODULE,
	  .objs = deku_objs,
  };
  `, prototypes, klpFunc, klpObjName))
	if err != nil {
		return err
	}

	// Append the contents of the MODULE_SUFFIX_FILE file to the end of the output file.
	suffix, _ := resources.ReadFile(MODULE_SUFFIX_FILE)
	_, err = outFile.Write(suffix)
	if err != nil {
		return err
	}

	return nil
}

func buildInKernel(srcFile string) bool {
	objPath, _ := findObjectFile(srcFile)
	return objPath != ""
}

func generateModule(file string) (dekuModule, error) {
	module := dekuModule{
		SrcFile: file,
		Name:    "",
		KoFile:  "",
	}
	baseName := filepath.Base(file)
	fileName := filenameNoExt(file)
	module.Name = generateModuleName(file)
	moduleDir := filepath.Join(config.workdir, module.Name)
	moduleId, _ := generateModuleId(file)
	module.KoFile = moduleDir + "/" + module.Name + ".ko"

	// Check if changed since last run
	prev, err := os.ReadFile(moduleDir + "/id")
	if err == nil && string(prev) == moduleId {
		return invalidateModule(module), nil
	}
	if !buildInKernel(file) {
		LOG_WARN("File '%s' is not used in the kernel or module. Skip", file)
		os.Mkdir(moduleDir, os.ModePerm)
		os.WriteFile(moduleDir+"/id", []byte(moduleId), 0644)
		os.Remove(module.KoFile)
		return invalidateModule(module), nil
	}

	LOG_INFO("Processing %s...", file)

	os.Mkdir(moduleDir, os.ModePerm)
	os.Remove(moduleDir + "/id")

	// Write diff to file for debug purpose
	diff, _ := getFileDiff(file)
	os.WriteFile(moduleDir+"/diff", diff, 0644)

	// File name with prefix '_' is the origin file
	if config.kernSrcInstallDir != "" {
		copyFile(config.kernSrcInstallDir+"/"+file, moduleDir+"/_"+baseName)
	}

	copyFile(config.sourceDir+"/"+file, moduleDir+"/"+baseName)
	os.WriteFile(moduleDir+"/"+FILE_SRC_PATH, []byte(file), 0644)

	err = buildFile(file, moduleDir+"/"+baseName, moduleDir+"/"+fileName+".o")
	if err != nil {
		LOG_ERR(err, "Error while build %s", file)
		return invalidateModule(module), err
	}

	modFuncs, err := generateDiffObject(moduleDir, file)
	if err != nil {
		LOG_ERR(err, "Error while finding modified functions in %s", file)
		return invalidateModule(module), err
	}
	if len(modFuncs) == 0 {
		LOG_INFO("No valid changes found in '%s'", file)
		os.Remove(module.KoFile)
		os.WriteFile(moduleDir+"/id", []byte(moduleId), 0644)
		return invalidateModule(module), nil
	}

	objPath, err := findObjectFile(file) // TODO pass to generateLivepatchSource
	if err != nil {
		LOG_ERR(err, "Can't find object file for %s", file)
		return invalidateModule(module), err
	}

	generateLivepatchSource(moduleDir, file, modFuncs)
	generateLivepatchMakefile(moduleDir+"/Makefile", file, module.Name)
	buildLivepatchModule(moduleDir)

	// Restore calls to origin func XYZ instead of __deku_XYZ
	for _, symbol := range modFuncs {

		plainSymbol := strings.ReplaceAll(symbol, ".", "_")
		err := changeCallSymbol(module.KoFile, DEKU_FUN_PREFIX+plainSymbol, plainSymbol)
		if err != nil {
			LOG_ERR(err, "Fail to change calls to %s in %s", plainSymbol, module.KoFile)
			return invalidateModule(module), err
		}

		stripSymbolArg := "--strip-symbol=" + DEKU_FUN_PREFIX + plainSymbol
		cmd := exec.Command("objcopy", stripSymbolArg, module.KoFile)
		cmd.Stderr = os.Stderr
		err = cmd.Run()
		if err != nil {
			LOG_ERR(err, "Fail to restore origin function names")
			return invalidateModule(module), err
		}
	}

	err = adjustRelocations(module, objPath, modFuncs)
	if err != nil {
		LOG_DEBUG("Fail to adjust relocations: %s", err)
		return invalidateModule(module), err
	}

	os.WriteFile(moduleDir+"/id", []byte(moduleId), 0644)

	return module, nil
}
