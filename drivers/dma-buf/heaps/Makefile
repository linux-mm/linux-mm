# SPDX-License-Identifier: GPL-2.0
obj-$(CONFIG_DMABUF_HEAPS_SYSTEM)	+= system_heap.o
obj-$(CONFIG_DMABUF_HEAPS_CMA)		+= cma_heap.o

#
# The reserved-memory bits always need to be built-in so that the CMA
# initialization runs during early boot. The VPR driver itself can be
# built as a module.
#
ifneq ($(CONFIG_DMABUF_HEAPS_TEGRA_VPR),)
obj-y					+= tegra-vpr-init.o
obj-$(CONFIG_DMABUF_HEAPS_TEGRA_VPR)	+= tegra-vpr.o
endif
