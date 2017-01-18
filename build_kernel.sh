#!/bin/bash

export ARCH=arm;
export CROSS_COMPILE=$(pwd)/../gcc/ubertc/arm-eabi-4.9/bin/arm-eabi-;
export CCOMPILE=$CROSS_COMPILE;
export CCACHE_DIR=~/.ccache;
#git clean -d -x -f && ccache -C -z
if ! [ -d "out" ]; then
mkdir out;
echo "output folder created!...";
else
echo "output folder exits!...";
fi
make clean && make mrproper;
make -C $(pwd) O=out msm8916_sec_defconfig VARIANT_DEFCONFIG=msm8916_sec_j5xlte_eur_defconfig SELINUX_DEFCONFIG=selinux_defconfig;
make -j4 -C $(pwd) O=out;

cp out/arch/arm/boot/Image $(pwd)/arch/arm/boot/zImage;

tools/dtbTool -s 2048 -o out/arch/arm/boot/dt.img -p out/scripts/dtc/ out/arch/arm/boot/dts/ -v;
