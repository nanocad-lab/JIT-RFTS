# JIT-RFTS: An Reliability-Bookkeeping Kernel Module for BeagleBone Black

## Build Instructions:

### Install TI SDK

* The setup is with a Ubuntu 12.04 host machine. Special modifications may be required for other OS versions.
* Download and install the TI AM335x SDK. 
<pre>
wget http://software-dl.ti.com/sitara_linux/esd/AM335xSDK/07_00_00_00/exports/ti-sdk-am335x-evm-07.00.00.00-Linux-x86-Install.bin
chmod +x ti-sdk-am335x-evm-07.00.00.00-Linux-x86-Install.bin
./ti-sdk-am335x-evm-07.00.00.00-Linux-x86-Install.bin
</pre>
* Follow the GUI to install the SDK
* Follow the documentation to setup your BeagleBone Black (boot option, TFTP, cross-compilation etc.)

### Import JIT-RFTS kernel extensions

* Copy JIT-RFTS kernel extensions to the kernel code
<pre>
cp -r ./kernel_module/* {sdk_install_path}/board-support/linux-3.12.10-ti2013.12.01/
</pre>
* Configure Linux kernel for BeagleBone Black
<pre>
cd {sdk_install_path}/board-support/linux-3.12.10-ti2013.12.01/
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- menuconfig
</pre>
* Load an alternate config file, select ./config_default
* Save an alternate config file: .config
* Build the kernel image
<pre>
./compile.sh
</pre>
