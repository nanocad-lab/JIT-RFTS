# JIT-RFTS: An Reliability-Bookkeeping Kernel Module for BeagleBone Black

##Build Instruction:

1. The setup is verified with a Ubuntu 12.04 machine. Special modifications may be required for other versions.

2. Download and install the TI AM335x SDK. 

```
wget http://software-dl.ti.com/sitara_linux/esd/AM335xSDK/07_00_00_00/exports/ti-sdk-am335x-evm-07.00.00.00-Linux-x86-Install.bin
chmod +x ti-sdk-am335x-evm-07.00.00.00-Linux-x86-Install.bin
./ti-sdk-am335x-evm-07.00.00.00-Linux-x86-Install.bin
```

3. Follow the GUI to install the SDK.

4. Follow the documentation to setup your BeagleBone Black.

5. Apply the patches by copying the files.

```
cp -r ./kernel_module/* {sdk_install_path}/board-support/linux-3.12.10-ti2013.12.01/
```

6. Config the kernel

```
cd {sdk_install_path}/board-support/linux-3.12.10-ti2013.12.01/
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- menuconfig
Load an alternate config file, select ./config_default
Save an alternate config file: .config
```

7. Build the kernel image

```
./compile.sh
```

The I/O of the kernel module is done through sysfs interface.
The virtual files are located in:

/sys/devices/system/cpu/cpu0/cpufreq/re_stats/

