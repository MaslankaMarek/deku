# DEKU for ChromiumOS

## Table of Contents
- [About the DEKU](#about)
- [Prerequisites](#prerequisites)
- [Init deku](#init)
- [Usage](#usage)

---

<a name="about"></a>
## About the DEKU for ChromiumOS
Since DEKU includes integrations for the ChromiumOS SDK, it can be used in an easier way.
For example, without performing synchronization on every kernel compilation.

<a name="prerequisites"></a>
## Prerequisites
 - Build the kernel with: `USE="livepatch kernel_sources" emerge-${BOARD} chromeos-kernel-...`
 - Flash the kernel to the device.

<a name="init"></a>
## Init DEKU
Download and build DEKU inside cros sdk environment
```bash
git clone https://github.com/Semihalf/deku.git
cd deku
make
```
Add DEKU to the `PATH` environment. Run the following command inside the `deku` dir
```bash
echo export PATH=\$PATH:`pwd` >> ~/.bashrc
source ~/.bashrc
```

Initialize DEKU for specific board
```bash
deku --board=<BOARD_NAME> -p root@<DUT_ADDRESS[:PORT]> init
```
Adjust:
- `--board=<BOARD_NAME>` board name. The meaning of this parameter is the same as in the ChromiumOS SDK,
- `-p root@<DUT_ADDRESS[:PORT]>` Chromebook address and optionally SSH port number.

Example usage:
`deku --board=brya -p root@192.168.0.100:22 init`

<a name="usage"></a>
## Usage
Use
```bash
deku --board=<BOARD_NAME> deploy
```
to apply changes to the kernel on the DUT.

Example usage:
`deku --board=brya deploy`

***
[Read the rest of the README](README.md#rest_of_readme)
