echo "Removing device from PCIe bus"
echo 1 | sudo tee /sys/bus/pci/devices/0000\:01\:00.0/remove 
echo "Rescanning"
echo 1 | sudo tee /sys/bus/pci/rescan