#
#  Makefile
#
#  Created on: March 11, 2023
#      Author: Dylan Robinson
#

ifneq ($(KERNELRELEASE),)

	snd-usb-motu-objs := motu-usb-audio.o

	# ccflags-y := -I./include
	# ccflags-y += -DMOTU

	obj-m += snd-usb-motu.o

else

KDIR ?= /lib/modules/`uname -r`/build
TARGET := snd-usb-motu.ko

default:
	$(MAKE) -C $(KDIR) M=$(shell pwd) modules

clean:
	$(MAKE) -C $(KDIR) M=$(shell pwd) clean

install:
	install -D $(TARGET) $(DESTDIR)/lib/modules/$(TARGET)

# DKMS targets - uses individual dkms install for each kernel
dkms_install:
	@echo "Installing $(TARGET) via DKMS (building for all kernels)..."
	@./dkms_install.sh

dkms_remove:
	@echo "Removing $(TARGET) from DKMS..."
	@sudo dkms remove $(TARGET:.ko=)/1.0 --all

dkms_status:
	@echo "DKMS status for $(TARGET):"
	@dkms status -m $(TARGET:.ko=)

# Verify DKMS installation for all kernels
dkms_verify:
	@echo "Verifying DKMS installation for all kernels..."
	@./dkms_verify.sh

.PHONY: default clean install dkms_install dkms_remove dkms_status dkms_verify

endif
