ifneq ($(KERNELRELEASE),)

# kbuild part of makefile
obj-m += davinci/
obj-m += generic/
subdir-y += arch/arm/boot/dts

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
	modprobe -r snd_soc_botic_codec
	modprobe -r snd_soc_davinci_mcasp

load:
	modprobe snd_soc_davinci_mcasp
	modprobe snd_soc_botic_codec
	modprobe snd_soc_botic

config:
	# configure botic arguments that are not set on the kernel command line
	echo "MDR-" > /sys/module/snd_soc_botic/parameters/serconfig
	echo 45158400 > /sys/module/snd_soc_botic/parameters/clk_44k1
	echo 49152000 > /sys/module/snd_soc_botic/parameters/clk_48k
	## BBB without external clocks
	#echo 0 > /sys/module/snd_soc_botic/parameters/clk_44k1
	#echo 24576000 > /sys/module/snd_soc_botic/parameters/clk_48k

reload: build install unload load config
	@echo Reloaded.
	@dmesg | tail -10

# Notice: this often fails if Ethernet cable is plugged into BBB during reboot
reload-dtb: build
	kexec -l --command-line="`cat /proc/cmdline | sed s/quiet//`" --dtb="arch/arm/boot/dts/am335x-boneblack-botic.dtb" /boot/vmlinuz-`uname -r`
	@sync
	@echo "Remove Ethernet cable..."
	sleep 10
	@echo "Restarting..."
	kexec -e -x

prepare: relink scripts

relink:
	find arch/arm/boot/dts -type l -print0 | xargs -0r rm
	find "$(KDIR)/arch/arm/boot/dts" -maxdepth 1 -name "*.dtsi" -print0 | xargs -0r ln -s -t arch/arm/boot/dts

scripts:
	$(MAKE) -C $(KDIR) scripts

endif
