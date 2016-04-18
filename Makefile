ifneq ($(KERNELRELEASE),)

# kbuild part of makefile
obj-m += davinci/
obj-m += generic/

else

# normal makefile
KDIR ?= /lib/modules/`uname -r`/build

build:
	$(MAKE) -C $(KDIR) M=$$PWD

install:
	$(MAKE) -C $(KDIR) M=$$PWD INSTALL_MOD_DIR=kernel/sound/soc modules_install
	depmod -A

clean:
	$(MAKE) -C $(KDIR) M=$$PWD clean

# Module specific targets
unload:
	modprobe -r snd_soc_botic
	modprobe -r snd_soc_davinci_mcasp

load:
	modprobe snd_soc_davinci_mcasp
	modprobe snd_soc_botic

config:
	# configure botic arguments that are not set on the kernel command line
	echo "MDC-" > /sys/module/snd_soc_botic/parameters/serconfig
	echo 45158400 > /sys/module/snd_soc_botic/parameters/clk_44k1
	echo 49152000 > /sys/module/snd_soc_botic/parameters/clk_48k

reload: build install unload load config
	@echo Reloaded.
	@dmesg | tail -10

endif
