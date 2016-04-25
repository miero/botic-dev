ifeq ($(CONFIG_OF),y)

ifeq ($(CONFIG_OF_OVERLAY),y)
DTC_FLAGS += -@
endif

dtb-$(CONFIG_SOC_AM33XX) += \
	am335x-boneblack-botic.dtb
endif

always		:= $(dtb-y)
clean-files	:= *.dtb
