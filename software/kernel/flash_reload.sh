#!/usr/bin/bash
echo $#
if [ "$#" -ne "1" ]; then
	echo "Missing bitstream path"
	exit
fi
BITSTREAM=$1

if [ ! -e $BITSTREAM ]; then
	echo "Error: cannot file $BITSTREAM"
	exit
fi

BUSDEV=$(lspci  | grep "RF controller" | cut -d' ' -f 1)
../user/m2sdr_util flash_write $BITSTREAM
../user/m2sdr_util flash_reload
sudo rmmod litepcie
echo 1 | sudo tee /sys/bus/pci/devices/0000\:$BUSDEV/remove
echo 1 | sudo tee /sys/bus/pci/rescan
sudo ./init.sh
