# DExterous Kernel Update [DEKU]

_**For ChromiumOS developers:**_

Go to [ChromiumOS developers README](README_CHROMIUMOS.md)
***

## Table of Contents
- [About the DEKU](#about)
- [Prerequisites](#prerequisites)
- [Download & build](#download)
- [Usage](#usage)
- [Supported kernel versions](#supportedversions)
- [Constraints](#constraints)

---

<a name="about"></a>
## About the DEKU
The DEKU is a utility that allows quick apply changes from Linux kernel source code to a running kernel on the device. DEKU is using the kernel livepatching feature to provide changes to a running kernel. This tool is primarily intended for Linux kernel developers, but it can also be useful for researchers to learn how the kernel works.
<a name="prerequisites"></a>
## Prerequisites
 - Install `libelf-dev`, `libiberty-dev`, `binutils-dev`, `golang`
 - Enable `CONFIG_LIVEPATCH` in the kernel config  
 The above flag depends on the `KALLSYMS_ALL` flag that isn't enabled by default.
 - SSH Key-Based authentication to the DUT

<a name="download"></a>
## Fetch & Build DEKU
Download and build using `make` command
```bash
git clone https://github.com/MaslankaMarek/deku.git
cd deku
git checkout experimental
make
```

<a name="usage"></a>
## Usage
```bash
./deku -b <PATH_TO_KERNEL_BUILD_DIR> --target <USER@DUT_ADDRESS[:PORT]> COMMAND
```
```
Commands list:
    build                                 build the DEKU modules which are livepatch kernel's modules,
    deploy                                build and deploy the changes to the device.
    sync                                  synchronize information about kernel source code.
                                          Use this command after building the kernel. The use of
                                          this command is not mandatory, but it will make DEKU work
                                          more reliably. When the --src_inst_dir parameter is used,
                                          it is unnecessary to execute this command after the kernel
                                          build, as DEKU runs more reliably with the --src_inst_dir
                                          parameter.

Available parameters:
    -b, --builddir                        path to kernel build directory,
    -s, --sourcesdir                      path to kernel sources directory. Use this parameter
                                          if DEKU can't find kernel sources dir,
    --target=<USER@DUT_ADDRESS[:PORT]>    SSH connection parameter to the target device. The given
                                          user must be able to load and unload kernel modules. The
                                          SSH must be configured to use key-based authentication.
                                          Below is an example with this parameter,
    --ssh_options=<"-o ...">              options for SSH. Below is an example with this parameter,
    --src_inst_dir=<PATH>                 directory with the kernel sources that were installed after
                                          the kernel was built. Having this directory makes DEKU
                                          working more reliable. As an alternative to this
                                          parameter, the 'deku sync' command can be executed after
                                          the kernel has been built to make DEKU work more reliably,
```

### Example usage
Use
```bash
./deku -b /home/user/linux_build --target=root@192.168.0.100:2200 deploy
```
to apply changes to the kernel on the DUT.

Use
```bash
./deku -b /home/user/linux_build --target=root@192.168.0.100 --ssh_options="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -i ~/key_rsa" deploy
```
when custom key-based authentication key is used for ssh connection

Use
```bash
./deku sync
```
command after building the kernel to make DEKU work more reliably. The use of this command is not mandatory when the `--src_inst_dir` parameter is used.

To generate kernel livepatch module without deploy it on the target use
```bash
./deku -b /home/user/linux_build build
```
command. Module can be found at `~/.cache/deku/workdir_XXXX/deku_YYYY/deku_YYYY.ko`

Changes applied in the kernel on the DUT are not persistent and are life until the next reboot. After every reboot, the `deploy` must be performed.

<a name="rest_of_readme"></a>

<a name="supportedversions"></a>
## Supported kernel versions
The minimum supported kernel version is: 5.4

<a name="constraints"></a>
## Constraints
 - Only changes in `.c` source file are supported. Changes in header files are not supported yet.
 - ARM and other architectures are not supported yet.
 - Functions marked as `__init`, `__exit` and `notrace` are not supported.
 - Functions that uses jump labels/static keys are not supported yet.
 - KLP relocations for non-unique symbols in modules are not supported yet.
 - Functions with non-unique name in the object file are not supported yet.
 - Kernel configurations with the CONFIG_OBJTOOL for stack validation are not fully supported yet.
 - Changes to the lib/* directory are not supported
