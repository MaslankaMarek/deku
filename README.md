# DEKU

## Table of Contents
[About the DEKU](#about)  
[Prerequisites](#prerequisites)  
[Init deku](#init)  
[Usage](#usage)  
[Constraints](#constraints)  

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
***
_**For ChromiumOS developers**_  
 - `libelf` is already installed in cros sdk
 - To enable `CONFIG_LIVEPATCH` flag the kernel can be built with the `livepatch`
 use flag
  ```
  USE=livepatch emerge-$BOARD chromeos-kernel-$VERSION
 ```
***
Build and upload kernel on the DUT

<a name="init"></a>
## Init DEKU
Download and go to deku directory
```
git clone https://github.com/Semihalf/deku.git
cd deku
make
```
***
_**For ChromiumOS developers**_  
Download DEKU inside cros sdk environment
***
In the deku directory use following command to initialize environment:
```
./deku -b <PATH_TO_KERNEL_BUILD_DIR> [-s <PATH_TO_KERNEL_SOURCES_DIR>] [--board=<CHROMEBOOK_BOARD_NAME>] -d ssh -p <USER@DUT_ADDRESS[:PORT]> init
```
`-b` path to the kernel build directory,  
`-s` path to the kernel sources directory. Use this parameter if the initialization process can't find kernel sources dir,  
`--board` *(Only available inside ChromiumOS SDK)* board name. The meaning of this parameter is the same as in the ChromiumOS SDK,  
`-d` method used to upload and deploy livepatch modules to the DUT. Currently, only `ssh` is supported,  
`-p` parameters for the deploy method. For the `ssh` deploy method, pass the user and DUT address. Optional pass the port number,  
The given user must be able to load and unload kernel modules. The SSH must be configured to use key-based authentication.


***
_**For ChromiumOS developers**_  
Use the `--board` parameter instead of `-b` to auto detect kernel build dir. 

Example usage:  
`./deku --board=atlas -d ssh -p root@192.168.0.100:22 init`

If for some reason the `--board` parameter can't be used the `-b` parameter with a kernel dir must be pass.  
Kernel build directory inside the cros sdk is located in `/build/${BOARD}/var/cache/portage/sys-kernel/chromeos-kernel-${KERNEL_VERSION}`
***

<a name="usage"></a>
## Usage
Use
```
make deploy
```
to apply changes to the kernel on the DUT.

In case the kernel will be rebuilt manually the DEKU must be synchronized with the new build.

Use
```
make sync
```
command to perform synchronization.

To generate kernel livepatch module without deploy it on the target use
```
make build
```
command. Modules can be found in `workdir/deku_XXXX/deku_XXXX.ko`

Changes applied in the kernel on the DUT are not persistent and are life until the next reboot. After every reboot, the `deploy` must be performed.

Instead of the `make` command, the `./deku` can be used. E.g.
```
./deku deploy
```
under the hood, the `make` command just calls the `deku` utility with the same parameters

<a name="constraints"></a>
## Constraints
 - Only changes in ".c" source file are supported. Changes in header files are not supported yet.
 - ARM and other architectures are not supported yet.
 - Functions marked as "__init" are not supported.
 - Functions that uses jump labels/static keys are not supported yet.
