# Makefile for kernel module
KERNEL_VERSION := $(shell uname -r)
KERNEL_PATH ?= /lib/modules/$(KERNEL_VERSION)/build

obj-m = m2sdr.o
m2sdr-objs = main.o

# Default target
all: m2sdr.ko

# Build the module
m2sdr.ko: main.c litepcie.h config.h flags.h csr.h soc.h
	make -C $(KERNEL_PATH) M=$(shell pwd) modules

# Install the module to the kernel directory, enable auto-load on boot, and install udev rule to set
# device permissions.
install:
	make -C $(KERNEL_PATH) M=$(shell pwd) modules_install
	@echo m2sdr | sudo tee /etc/modules-load.d/m2sdr.conf
	sudo depmod -a
	@echo 'KERNEL=="m2sdr*", MODE="0666"' | sudo tee /etc/udev/rules.d/99-m2sdr.rules
	sudo udevadm control --reload-rules && sudo udevadm trigger

# Uninstall the module and remove configuration files
uninstall:
	-sudo rmmod m2sdr || true
	sudo rm -f /lib/modules/$(KERNEL_VERSION)/updates/m2sdr.ko
	sudo rm -f /etc/modules-load.d/m2sdr.conf
	sudo rm -f /etc/udev/rules.d/99-m2sdr.rules
	sudo depmod

# Clean up build artifacts
clean:
	make -C $(KERNEL_PATH) M=$(shell pwd) clean
	rm -f *~

.PHONY: all clean install uninstall
