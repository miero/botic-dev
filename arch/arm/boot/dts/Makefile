ifeq ($(CONFIG_OF),y)

ifeq ($(CONFIG_OF_OVERLAY),y)
DTC_FLAGS += -@
endif

srctree=$(KDIR)
DTC_FLAGS += -i $(KDIR)/arch/arm/boot/dts

dtb-$(CONFIG_SOC_AM33XX) += \
	am335x-boneblack-botic.dtb \
	am335x-boneblack-botic-sabre32.dtb
endif

always		:= $(dtb-y)
clean-files	:= *.dtb
