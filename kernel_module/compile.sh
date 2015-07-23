#make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- tisdk_am335x-evm_defconfig -j4
#make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- customized_defconfig -j4
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- zImage -j4
#make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- am335x-boneblack.dtb -j4
