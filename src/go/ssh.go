package main

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
)

func getRemoteParameters(forSSH bool) []string {
	host := config.deployParams
	if strings.Contains(host, "@") {
		host = strings.Split(host, "@")[1]
	}
	host = strings.Split(host, ":")[0]

	args := []string{
		"-o", "ControlMaster=auto",
		"-o", "ControlPersist=300"}
	sshOpts := strings.TrimSpace(config.sshOptions)
	if sshOpts != "" {
		args = append(args, strings.Split(sshOpts, " ")...)
	}
	deployParams := config.deployParams
	if strings.Contains(deployParams, " ") {
		deployParams = strings.SplitN(config.deployParams, " ", 2)[0]
	}
	colonPos := strings.Index(deployParams, ":")
	port := ""
	if colonPos != -1 {
		port = deployParams[colonPos+1:]
		deployParams = deployParams[:colonPos]
		if forSSH {
			args = append(args, "-p", port)
		} else {
			args = append(args, "-P", port)
		}
	}
	args = append(args, "-o", "ControlPath=/tmp/ssh-deku-"+host+string(port))
	return append(args, deployParams)
}

func runSSHCommand(command string) (string, error) {
	cmd := exec.Command("ssh")
	cmd.Args = append(cmd.Args, getRemoteParameters(true)...)
	cmd.Args = append(cmd.Args, command)
	out, err := cmd.CombinedOutput()
	LOG_DEBUG("%s\n%s", cmd.String(), string(out))

	if exiterr, e := err.(*exec.ExitError); e {
		err = mkError(exiterr.ExitCode())
	}

	return strings.TrimSuffix(string(out), "\n"), err
}

func uploadFiles(files []string) error {
	var dstdir = "deku"
	cmd := exec.Command("scp")
	params := getRemoteParameters(false)
	host := params[len(params)-1]
	params = params[:len(params)-1]
	cmd.Args = append(cmd.Args, params...)
	cmd.Args = append(cmd.Args, files...)
	cmd.Args = append(cmd.Args, host+":"+dstdir+"/")
	out, err := cmd.CombinedOutput()
	LOG_DEBUG("%s", cmd.String())
	LOG_DEBUG("%s", out)
	return err
}

func getRemoteKernelRelease() (string, error) {
	out, err := runSSHCommand("uname --kernel-release")
	return out, err
}

func getRemoteKernelVersion() (string, error) {
	out, err := runSSHCommand("uname --kernel-version")
	return out, err
}

func getLoadedDEKUModules() ([]string, error) {
	out, err := runSSHCommand(`find /sys/module -name .note.deku -type f -exec cat {} \; | grep -a deku_ 2>/dev/null`)
	if len(out) == 0 {
		return []string{}, err
	}
	return strings.Split(out, "\n"), err
}

func startDaemon() error {
	// _, err := runSSHCommand("")
	// return err
	return nil
}

func generateLoadScript(modulesToLoad, modulesToUnload []dekuModule) (string, error) {
	allModules := append(modulesToLoad, modulesToUnload...)
	var dstdir = "deku"
	var unload string
	var checkmod string
	var insmod string
	var reloadscript = ""
	var checkTransition = config.kernelVersion >= versionNum(5, 10, 0) // checking patch transition in not realiable on kernel <5.10

	reloadscript += "INSMOD=insmod\n"
	reloadscript += "RMMOD=rmmod\n"
	reloadscript += "if [ ! -z \"$EUID\" ] && [ \"$EUID\" -ne 0 ]; then\n"
	reloadscript += "\tINSMOD=\"sudo insmod\"\n"
	reloadscript += "\tRMMOD=\"sudo rmmod\"\n"
	reloadscript += "fi\n"

	for _, module := range modulesToLoad {
		moduleDir := filepath.Join(config.workdir, module.Name)
		objfile, _ := os.ReadFile(filepath.Join(moduleDir, FILE_OBJECT_PATH))
		if strings.HasSuffix(string(objfile), ".ko") {
			srcfile, _ := os.ReadFile(filepath.Join(moduleDir, FILE_SRC_PATH))
			moddep := filenameNoExt(string(objfile))
			reloadscript += "\ngrep -q '\\b" + moddep + "\\b' /proc/modules\n"
			reloadscript += "if [ $? != 0 ]; then\n"
			reloadscript += "\techo \"Can't apply changes for " + string(srcfile) +
				" because the '" + moddep + "' module is not loaded\""
			reloadscript += "\n\texit " + fmt.Sprintf("%d", ERROR_DEPEND_MODULE_NOT_LOADED) + "\n"
			reloadscript += "fi\n"
		}
	}

	for _, module := range allModules {
		modulename := strings.ReplaceAll(module.Name, "-", "_")
		modulesys := fmt.Sprintf("/sys/kernel/livepatch/%s", modulename)

		checkmod += "[ ! -d " + modulesys + " ] || \\"

		reloadscript += "[ -d " + modulesys + " ] && echo 0 > " + modulesys + "/enabled\n"

		unload += "for i in $(seq 1 150); do\n"
		unload += "\t[ ! -d " + modulesys + " ] && break\n"
		unload += "\t[ $(cat " + modulesys + "/transition) = \"0\" ] && break\n"
		unload += "\t[ $(($i%25)) = 0 ] && echo \"Undoing previous changes made to " + module.SrcFile + " is still in progress ...\"\n"
		unload += "\tsleep 0.2\ndone\n"

		unload += "[ -d /sys/module/" + modulename + " ] && $RMMOD " + modulename + "\n"

		unload += "for i in $(seq 1 250); do\n"
		unload += "\t[ ! -d " + modulesys + " ] && break\n"
		unload += "\t[ $(($i%25)) = 0 ] && echo \"Cleaning up after previous changes to " + module.SrcFile + " is still in progress...\"\n"
		unload += "\tsleep 0.2\n"
		unload += "done\n"
	}

	for _, module := range modulesToLoad {
		modulename := strings.ReplaceAll(module.Name, "-", "_")
		modulesys := fmt.Sprintf("/sys/kernel/livepatch/%s", modulename)

		insmod += "module=" + module.Name + "\n"
		insmod += "res=$($INSMOD " + filepath.Join(dstdir, "$module.ko") + " 2>&1)\n"
		insmod += "if [ $? != 0 ]; then\n"
		insmod += "\techo \"Failed to load changes for " + module.SrcFile + ". Reason: $res\"\n"
		insmod += "\texit " + fmt.Sprintf("%d", ERROR_LOAD_MODULE) + "\n"
		insmod += "fi\n"
		insmod += "for i in $(seq 1 50); do\n"
		insmod += "\tgrep -q " + modulename + " /proc/modules && break\n"
		insmod += "\t[ $? -ne 0 ] && { echo \"Failed to load module " + modulename + "\"; exit " + fmt.Sprintf("%d", ERROR_LOAD_MODULE) + "; }\n"
		insmod += "\techo \"" + modulename + " module is still loading...\"\n"
		insmod += "\tsleep 0.2\ndone\n"
		if checkTransition {
			insmod += "for i in $(seq 1 150); do\n"
			insmod += "\t[ $(cat " + modulesys + "/transition) = \"0\" ] && break\n"
			insmod += "\t[ $(($i%25)) = 0 ] && echo \"Applying changes for " + module.SrcFile + " is still in progress...\"\n"
			insmod += "\tsleep 0.2\ndone\n"
			insmod += "[ $(cat " + modulesys + "/transition) != \"0\" ] && { echo \"Failed to apply " + modulename + " $i\"; exit " + fmt.Sprintf("%d", ERROR_APPLY_KLP) + "; }\n"
		} else {
			insmod += "sleep 2\n"
		}
		insmod += "echo \"" + module.SrcFile + " done\"\n"
	}

	reloadscript += "\n" + unload + "\n" + checkmod + "\n\t{ echo \"Previous changes cannot be undone\"; exit " + fmt.Sprintf("%d", ERROR_LOAD_MODULE) + "; }\n"
	reloadscript += "\n" + insmod

	scriptPath := config.workdir + DEKU_RELOAD_SCRIPT
	err := os.WriteFile(scriptPath, []byte(reloadscript), 0644)
	if err != nil {
		LOG_ERR(err, "Failed to write the reload script")
		return "", err
	}
	return scriptPath, err
}

func uploadAndLoadModules(modulesToLoad, modulesToUnload []dekuModule) {
	LOG_DEBUG("Modules to load: %s", modulesToLoad)
	LOG_DEBUG("Modules to unload: %s", modulesToUnload)

	var dstdir = "deku"
	scriptPath, err := generateLoadScript(modulesToLoad, modulesToUnload)
	if err != nil {
		return
	}
	if _, err := runSSHCommand("mkdir -p " + dstdir); err != nil {
		os.Exit(errorStrToCode(err))
	}

	filesToUpload := []string{}
	for _, module := range modulesToLoad {
		filesToUpload = append(filesToUpload, module.KoFile)
	}
	filesToUpload = append(filesToUpload, scriptPath)

	uploadFiles(filesToUpload)

	if len(modulesToLoad) > 0 {
		LOG_INFO("Loading...")
	} else {
		LOG_INFO("Reverting...")
	}

	out, err := runSSHCommand("sh " + dstdir + "/" + DEKU_RELOAD_SCRIPT + " 2>&1")
	if err == nil {
		LOG_INFO("%sChanges successfully applied!%s", GREEN, NC)
	} else {
		LOG_INFO("----------------------------------------")
		LOG_INFO("%s", out)
		LOG_INFO("----------------------------------------")
		LOG_INFO("Failed to apply changes!")
		LOG_INFO("Check the system logs on the device for more information.")
		os.Exit(errorStrToCode(err))
	}
}
