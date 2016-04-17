ifneq ($(KERNELRELEASE),)

# kbuild part of makefile
obj-m				:= snd-soc-botic.o snd-soc-davinci-mcasp.o
snd-soc-davinci-mcasp-objs	:= davinci-mcasp.o
snd-soc-botic-objs		:= botic-card.o

else

# normal makefile
KDIR ?= /lib/modules/`uname -r`/build

default:
	$(MAKE) -C $(KDIR) M=$$PWD

install:
	$(MAKE) -C $(KDIR) M=$$PWD modules_install
	depmod -A

# Module specific targets
reload: install
	modprobe -r snd_soc_botic
	modprobe -r snd_soc_davinci_mcasp
	modprobe snd_soc_davinci_mcasp
	modprobe snd_soc_botic
	@echo Reloaded.

endif
