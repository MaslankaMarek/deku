package main

import (
	"bufio"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
)

func cmdFromMod(modFile string, skipParam []string) ([]string, string, error) {
	var cmd []string
	var extraCmd string
	var line string

	file, err := os.Open(modFile)
	if err != nil {
		return cmd, extraCmd, err
	}
	defer file.Close()

	scanner := bufio.NewScanner(file)

	if scanner.Scan() {
		line = scanner.Text()
	}

	array := strings.SplitN(line, "=", 2)
	if len(array) < 2 {
		return cmd, extraCmd, fmt.Errorf("can't find command in %s", modFile)
	}
	line = array[1]
	parts := strings.SplitN(line, ";", 2)
	if len(parts) == 2 {
		line = parts[0]
		extraCmd = parts[1]
	}

	params := strings.Fields(line)

	for i := 0; i < len(params); i++ {
		opt := params[i]

		if strings.Contains(opt, "=") {
			param := strings.SplitN(opt, "=", 2)[0]
			if !slicesContains(skipParam, param) {
				cmd = append(cmd, opt)
			}
		} else {
			if !slicesContains(skipParam, opt) {
				cmd = append(cmd, opt)
			} else {
				i++
			}
		}
	}
	return cmd, extraCmd, nil
}

func cmdBuildFile(srcFile string) ([]string, string, error) {
	file := filenameNoExt(srcFile)
	dir := filepath.Dir(srcFile)
	modFile := filepath.Join(config.buildDir, dir, fmt.Sprintf(".%s.o.cmd", file))

	if _, err := os.Stat(modFile); err != nil {
		return nil, "", err
	}

	skipParam := []string{"-o", "-Wdeclaration-after-statement"}
	cmd, extraCmd, err := cmdFromMod(modFile, skipParam)
	if err != nil {
		return nil, "", err
	}

	cmd = cmd[:len(cmd)-1]
	cmd = append(cmd, "-I"+config.sourceDir+dir)

	return cmd, extraCmd, err
}

func buildFile(srcFile, compileFile, outFile string) error {
	var cmd []string
	var extraCmd string

	cmd, extraCmd, err := cmdBuildFile(srcFile)
	if err != nil {
		return nil
	}

	currentPath, _ := os.Getwd()
	if !filepath.IsAbs(outFile) {
		outFile = filepath.Join(currentPath, outFile)
	}

	if !filepath.IsAbs(compileFile) {
		compileFile = filepath.Join(currentPath, compileFile)
	}

	cmd = append(cmd, "-o", outFile, compileFile)

	execCmd := exec.Command("bash", "-c", strings.Join(cmd, " ")) // FIXME:Do not use a bash
	execCmd.Stdout = os.Stdout
	execCmd.Stderr = os.Stderr
	execCmd.Dir = config.linuxHeadersDir
	err = execCmd.Run()

	if err != nil {
		LOG_ERR(err, "Failed to build %s", srcFile)
		return err
	}

	if len(extraCmd) > 0 && false {
		if strings.HasPrefix(extraCmd, "./tools/objtool/objtool") && strings.HasSuffix(extraCmd, ".o") {
			array := strings.Split(extraCmd, " ")
			newExtraCmd := array[:len(array)-1]
			newExtraCmd = append(newExtraCmd, outFile)
			newExtraCmdStr := strings.Join(newExtraCmd, " ")

			LOG_DEBUG("Run extra command to build file: %s", newExtraCmdStr)

			execCmd := exec.Command(newExtraCmdStr)
			execCmd.Stdout = os.Stdout
			execCmd.Stderr = os.Stderr
			execCmd.Dir = config.buildDir

			if err := execCmd.Run(); err != nil {
				LOG_INFO("Failed to perform additional action for %s (%s)", srcFile, newExtraCmdStr)
			}
		} else {
			LOG_INFO("Can't parse additional command to build file (%s)", extraCmd)
		}
	}

	return nil
}

func buildModules(moduleDir string) error {
	cmd := exec.Command("make")
	cmd.Dir = moduleDir
	if config.useLLVM != "" {
		cmd.Args = append(cmd.Args, config.useLLVM)
	}

	out, err := cmd.CombinedOutput()
	if err != nil {
		LOG_ERR(err, "%s", string(out))
		return err
	}

	fileLog, _ := os.Create(filepath.Join(moduleDir, "build.log"))
	defer fileLog.Close()
	fileLog.Write(out)

	rc := len(out)
	if rc != 0 && err != nil {
		regexErr := `^.+\(\/\w+\.\w+\):\([0-9]\+\):[0-9]\+:.* error: \(.\+\)`
		errorCatched := false
		for _, line := range strings.Split(string(out), "\n") {
			err := regexp.MustCompile(regexErr).FindStringSubmatch(line)
			if err != nil {
				file := err[1]
				no, _ := strconv.Atoi(err[2])
				err := err[3]
				errorCatched = true
				fmt.Printf("%s:%d %serror:%s %s. See more: %s\n", file, no, RED, NC, err, "fileLog")
				break
			}
		}

		if !errorCatched {
			fmt.Println("Error:")
			fmt.Println(string(out))
		}

		// Remove the "_filename.o" file because next build might fails
		// TODO: Remove file directly by name
		_ = filepath.Walk(moduleDir, func(path string, info os.FileInfo, err error) error {
			if err != nil {
				return err
			}

			if info.Name() == "_*.o" {
				err := os.Remove(path)
				if err != nil {
					return err
				}
			}

			return nil
		})

		return errors.New("build failed")
	}

	return nil
}

func buildLivepatchModule(moduleDir string) error {
	fileLog := filepath.Join(moduleDir, "build.log")
	oldFileLog := filepath.Join(moduleDir, "build_modules.log")
	os.Rename(fileLog, oldFileLog)
	return buildModules(moduleDir)
}
