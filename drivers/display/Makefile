# SPDX-License-Identifier: GPL-2.0
# Out-of-tree Kbuild Makefile for Zinwa Q25 Display Panel & Bias IC drivers

ccflags-y += -I$(src)/include
ccflags-$(CONFIG_DRM_MEDIATEK_V2) += -I$(srctree)/drivers/gpu/drm/mediatek/mediatek_v2/

obj-m += ocp2131_drv.o
obj-m += panel-q20-hd720-lcm-dsi-vdo.o
