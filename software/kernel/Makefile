# Makefile for kernel module
KERNEL_VERSION := $(shell uname -r)
KERNEL_PATH ?= /lib/modules/$(KERNEL_VERSION)/build

obj-m = litepcie.o
litepcie-objs = main.o

# Default target
all: litepcie.ko

# Build the module
litepcie.ko: main.c litepcie.h config.h flags.h csr.h soc.h
	make -C $(KERNEL_PATH) M=$(shell pwd) modules

# Install the module to the kernel directory and enable auto-load on boot
install:
	make -C $(KERNEL_PATH) M=$(shell pwd) modules_install
	@echo litepcie | sudo tee /etc/modules-load.d/litepcie.conf
	sudo depmod -a

# Uninstall the module
uninstall:
	-sudo rmmod litepcie || true
	sudo rm -f /lib/modules/$(KERNEL_VERSION)/updates/litepcie.ko
	sudo rm -f /etc/modules-load.d/litepcie.conf
	sudo depmod

# Clean up build artifacts
clean:
	make -C $(KERNEL_PATH) M=$(shell pwd) clean
	rm -f *~

.PHONY: all clean install uninstall
