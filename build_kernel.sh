#!/bin/bash

zip_name="cherry";
DATETIME=`date +%D%H%M | sed 's-/--g'`;
zarch=""$zip_name"_"$DATETIME"";

export ARCH=arm;
export CROSS_COMPILE=$(pwd)/../linaro/arm-eabi-6.3.1/bin/arm-eabi-;

#git clean -d -x -f && ccache -C -z
if ! [ -d "out" ]; then
mkdir out;
echo "output folder created!...";
else
echo "output folder exits!...";
fi
while true; do
    read -p "Do you wish clean the source? " yn
    case $yn in
        [Yy]* ) make clean && make mrproper; break;;
        [Nn]* ) break;;
        * ) echo "Please answer yes or no.";;
    esac
done

echo "Get rid of the .orig and .rej files";
find .  -name '*.orig' -exec rm -f {} ';'
find .  -name '*.rej' -exec rm -f {} ';'

make -C $(pwd) O=out msm8916_sec_defconfig VARIANT_DEFCONFIG=msm8916_sec_j5xlte_eur_defconfig SELINUX_DEFCONFIG=selinux_defconfig;
make -j4 -C $(pwd) O=out;

#cp out/arch/arm/boot/Image $(pwd)/arch/arm/boot/zImage;

tools/dtbTool -s 2048 -o out/arch/arm/boot/dt.img -p out/scripts/dtc/ out/arch/arm/boot/dts/ -v;

cp out/arch/arm/boot/zImage ~/cherry
cp out/arch/arm/boot/dt.img ~/cherry
 
find . -name '*ko' -exec cp '{}' ~/cherry/modules/ \;
 
cd ~/cherry

zip -r $zarch ./
mv $zarch.zip ~/
