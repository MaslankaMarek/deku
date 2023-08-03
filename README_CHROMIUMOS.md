# # DExterous Kernel Update [DEKU] for ChromiumOS

## Table of Contents
- [About the DEKU](#about)
- [Prerequisites](#prerequisites)
- [Init deku](#init)
- [Usage](#usage)

---

<a name="about"></a>
## About the DEKU for ChromiumOS
Since DEKU includes integrations for the ChromiumOS SDK, it can be used in an easier way.
For example, without initializing and without performing synchronization on every kernel compilation.

<a name="prerequisites"></a>
## Prerequisites
 - Build the kernel with:  
 `USE="livepatch kernel_sources" emerge-${BOARD} chromeos-kernel-${KERNEL_VERSION}`
 - Flash the kernel to the device.

<a name="init"></a>
## Download and build DEKU
Download and build DEKU inside cros sdk environment
```bash
git clone https://github.com/Semihalf/deku.git
cd deku
make
```
Optionally, add DEKU to the `PATH` environment. Run the following command in the `deku` directory
```bash
echo export PATH=\$PATH:`pwd` >> ~/.bashrc
source ~/.bashrc
```

<a name="usage"></a>
## Usage
Use following command to apply changes to the kernel on the DUT.
```bash
deku --board=<BOARD_NAME> --target=<DUT_ADDRESS[:PORT]> deploy
```

Adjust:
- `--board=<BOARD_NAME>` board name. The meaning of this parameter is the same as in the ChromiumOS SDK,
- `--target=<DUT_ADDRESS[:PORT]>` Chromebook address and optionally SSH port number.

### Example use:  
`deku --board=brya --target=192.168.0.100:22 deploy`

***
[Read the rest of the README](README.md#rest_of_readme)
