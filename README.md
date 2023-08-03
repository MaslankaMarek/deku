# DEKU

_**For ChromiumOS developers:**_

Go to [ChromiumOS developers README](README_CHROMIUMOS.md)
***

## Table of Contents
- [About the DEKU](#about)
- [Prerequisites](#prerequisites)
- [Init deku](#init)
- [Usage](#usage)
- [Constraints](#constraints)

---

<a name="about"></a>
## About the DEKU
The DEKU is a utility that allows quick apply changes from Linux kernel source code to a running kernel on the device. DEKU is using the kernel livepatching feature to provide changes to a running kernel. This tool is primarily intended for Linux kernel developers, but it can also be useful for researchers to learn how the kernel works.
<a name="prerequisites"></a>
## Prerequisites
 - Install `libelf`
 - Enable `CONFIG_LIVEPATCH` in kernel config  
 The above flag depends on the `KALLSYMS_ALL` flag that isn't enabled by default.
 - SSH Key-Based authentication to the DUT

<a name="init"></a>
## Init DEKU
Download and go to deku directory
```
git clone https://github.com/Semihalf/deku.git
cd deku
make
```

In the deku directory use following command to initialize environment:
```bash
./deku -b <PATH_TO_KERNEL_BUILD_DIR> [-s <PATH_TO_KERNEL_SOURCES_DIR>] -d ssh -p <USER@DUT_ADDRESS[:PORT]> init
```
`-b` path to the kernel build directory,  
`-s` path to the kernel sources directory. Use this parameter if the initialization process can't find kernel sources dir,  
`-d` method used to upload and deploy livepatch modules to the DUT. Currently, only `ssh` is supported,  
`-p` parameters for the deploy method. For the `ssh` deploy method, pass the user and DUT address. Optional pass the port number,  
The given user must be able to load and unload kernel modules. The SSH must be configured to use key-based authentication.

<a name="usage"></a>
## Usage
Use
```bash
./deku deploy
```
to apply changes to the kernel on the DUT.

In case the kernel will be rebuilt manually the DEKU must be synchronized with the new build.

Use
```bash
./deku sync
```
command to perform synchronization.

To generate kernel livepatch module without deploy it on the target use
```bash
./deku build
```
command. Modules can be found in `workdir/deku_XXXX/deku_XXXX.ko`

Changes applied in the kernel on the DUT are not persistent and are life until the next reboot. After every reboot, the `deploy` must be performed.

### Use another kernel/device
If you are going to using DEKU with another kernel or device, you will need to download a new DEKU repository and perform a new init process.

<a name="rest_of_readme"></a>

<a name="constraints"></a>
## Constraints
 - Only changes in `.c` source file are supported. Changes in header files are not supported yet.
 - ARM and other architectures are not supported yet.
 - Functions marked as `__init`, `__exit` and `notrace` are not supported.
 - Functions that uses jump labels/static keys are not supported yet.
 - KLP relocations for non-unique symbols in modules are not supported yet.
 - Functions containing `__read_mostly` are not supported yet.
 - Optimized functions with the `.cold` suffix are not supported yet.
 - Functions with non-unique name in the object file are not supported yet.
 - Kernel configurations with the CONFIG_OBJTOOL for stack validation are not supported yet.
