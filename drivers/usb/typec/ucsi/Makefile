# SPDX-License-Identifier: GPL-2.0
CFLAGS_trace.o				:= -I$(src)

obj-$(CONFIG_TYPEC_UCSI)		+= typec_ucsi.o

typec_ucsi-y				:= ucsi.o

typec_ucsi-$(CONFIG_DEBUG_FS)		+= debugfs.o

typec_ucsi-$(CONFIG_TRACING)		+= trace.o

ifneq ($(CONFIG_POWER_SUPPLY),)
	typec_ucsi-y			+= psy.o
endif

ifneq ($(CONFIG_TYPEC_DP_ALTMODE),)
	typec_ucsi-y			+= displayport.o
endif

obj-$(CONFIG_UCSI_ACPI)			+= ucsi_acpi.o
obj-$(CONFIG_UCSI_CCG)			+= ucsi_ccg.o
obj-$(CONFIG_UCSI_STM32G0)		+= ucsi_stm32g0.o
obj-$(CONFIG_UCSI_PMIC_GLINK)		+= ucsi_glink.o
obj-$(CONFIG_CROS_EC_UCSI)		+= cros_ec_ucsi.o
obj-$(CONFIG_UCSI_LENOVO_YOGA_C630)	+= ucsi_yoga_c630.o
obj-$(CONFIG_UCSI_HUAWEI_GAOKUN)	+= ucsi_huawei_gaokun.o
