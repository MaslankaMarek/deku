package main

import (
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
)

type Cros struct {
	afdoFiles []string
}

func CrosKernelName(baseDir string, config Config) (string, error) {
	kernelsDir := filepath.Join(baseDir, "/build/", config.crosBoard, "/var/db/pkg/sys-kernel")
	kernels, _ := filepath.Glob(filepath.Join(
		kernelsDir,
		"chromeos-kernel-*"))
	if len(kernels) == 0 {
		LOG_ERR(nil, "Kernel must be build with: USE=\"livepatch kernel_sources\" emerge-%s chromeos-kernel-...", config.crosBoard)
		return "", errors.New("ERROR_INSUFFICIENT_BUILD_PARAMS")
	}

	return strings.TrimSuffix(filepath.Base(kernels[0]), "-9999"), nil
}

func (cros *Cros) extractAfdo(afdoPath, dstDir, afdoFile string) {
	baseDir := ""
	LOG_DEBUG("Extract afdo profile file (%s)", afdoPath)

	dstFile := fmt.Sprintf("%s/%s.xz", dstDir, afdoFile)
	os.MkdirAll(dstDir, 0755)
	copyFile(afdoPath, dstFile)

	cmd := exec.Command("xz", "--decompress", dstFile)
	cmd.Stderr = os.Stderr
	cmd.Stdout = os.Stdout
	cmd.Run()

	cmd = exec.Command(baseDir+"/usr/bin/llvm-profdata", "merge", "-sample", "-extbinary", "-output="+dstDir+"/"+afdoFile+".extbinary.afdo", dstDir+"/"+afdoFile)
	cmd.Stderr = os.Stderr
	cmd.Stdout = os.Stdout
	cmd.Run()

	cros.afdoFiles = append(cros.afdoFiles, dstFile)
}

func (cros *Cros) preBuild() {
	LOG_DEBUG("Run cros pre build")
	afdoFile := ""
	afdoPath := ""
	dstDir := ""
	baseDir := ""
	kerndir, err := CrosKernelName(baseDir, config)
	if err != nil {
		return
	}
	cros.afdoFiles = []string{}

	dstDir = fmt.Sprintf("%s/build/%s/tmp/portage/sys-kernel/%s-9999/work", baseDir, config.crosBoard, kerndir)
	ebuild, _ := os.ReadFile(filepath.Join(baseDir, "/build/", config.crosBoard, "/var/db/pkg/sys-kernel/", kerndir+"-9999", kerndir+"-9999.ebuild"))
	match := regexp.MustCompile(`(\w+\s)?AFDO_PROFILE_VERSION="(.*)"\n`)
	result := match.FindStringSubmatch(string(ebuild))
	if len(result) == 0 {
		LOG_DEBUG("Can't find afdo profile file")
		return
	}

	fileName := result[2]
	if fileName == "" {
		LOG_DEBUG("Afdo profile file is not specified")
		baseDir := "/mnt/host/source/"
		filepath.Walk(baseDir+".cache/distfiles/", func(path string, _ os.FileInfo, _ error) error {
			if strings.HasSuffix(path, ".afdo.xz") || strings.HasSuffix(path, ".gcov.xz") {
				LOG_DEBUG("Found %s", path)
				afdoFile = strings.TrimSuffix(filepath.Base(path), ".xz")
				cros.extractAfdo(path, dstDir, afdoFile)
			}
			return nil
		})
		return
	}

	afdoFile = fmt.Sprintf("%s-%s.afdo", kerndir, fileName)
	afdoPath = fmt.Sprintf("%s/var/cache/chromeos-cache/distfiles/%s.xz", baseDir, afdoFile)
	if fileExists(afdoPath) {
		cros.extractAfdo(afdoPath, dstDir, afdoFile)
		return
	}

	afdoFile = fmt.Sprintf("%s-%s.gcov", kerndir, fileName)
	afdoPath = fmt.Sprintf("%s/var/cache/chromeos-cache/distfiles/%s.xz", baseDir, afdoFile)
	if fileExists(afdoPath) {
		cros.extractAfdo(afdoPath, dstDir, afdoFile)
		return
	}

	afdoPath = fmt.Sprintf("%s/build/%s/tmp/portage/sys-kernel/%s-9999/distdir/%s.xz", baseDir, config.crosBoard, kerndir, afdoFile)
	if fileExists(afdoPath) {
		cros.extractAfdo(afdoPath, dstDir, afdoFile)
		return
	}

	afdoPath = fmt.Sprintf("%s/.cache/distfiles/%s.xz", baseDir, afdoFile)
	if fileExists(afdoPath) {
		cros.extractAfdo(afdoPath, dstDir, afdoFile)
		return
	}

	LOG_WARN("Can't find afdo profile file")
}

func (cros Cros) postBuild() {
	LOG_DEBUG("Run cros post build")
	for _, file := range cros.afdoFiles {
		os.Remove(strings.TrimSuffix(file, ".xz"))
	}
}

func spawnInCros() error {
	args := []string{}
	for i := 0; i < len(os.Args); i++ {
		arg := os.Args[i]
		if strings.HasPrefix(arg, "--cros_sdk") || arg == "-c" {
			if !strings.Contains(arg, "=") {
				i++
			}
			continue
		}
		args = append(args, arg)
	}
	LOG_DEBUG("Entering to cros_sdk with: %s", strings.Join(args, " "))

	cros_sdk := "/usr/local/google/home/mmaslanka/chromeos/"
	cwd := "~/chromeos/deku"

	s := `cd ` + cros_sdk + `
mount --bind . chroot/mnt/host/source/
mount --bind .cache chroot/var/cache/chromeos-cache
mount -n --bind ` + cros_sdk + `/out/tmp chroot/tmp
mkdir -p chroot/tmp/deku
mount --bind ` + cwd + ` chroot/tmp/deku

cat << EOF | chroot chroot/
cd /tmp/deku
` + strings.Join(args, " ") + `
EOF`
	cmd := exec.Command("unshare", "--user", "--mount", "--map-root-user",
		"--fork", "bash", "-c", s)
	cmd.Stderr = os.Stderr
	cmd.Stdout = os.Stdout
	cmd.Run()

	os.Exit(0)

	return nil
}
