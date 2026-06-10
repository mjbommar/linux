# SPDX-License-Identifier: GPL-2.0
# 
# Copyright (C) 2000, 2002, 2003 Jeff Dike (jdike@karaya.com)
#

# pcap is broken in 2.5 because kbuild doesn't allow pcap.a to be linked
# in to pcap.o

vector-objs := vector_kern.o vector_user.o vector_transports.o
mconsole-objs := mconsole_kern.o mconsole_user.o
hostaudio-objs := hostaudio_kern.o
ubd-objs := ubd_kern.o ubd_user.o
port-objs := port_kern.o port_user.o
harddog-objs := harddog_kern.o
harddog-builtin-$(CONFIG_UML_WATCHDOG) := harddog_user.o harddog_user_exp.o
rtc-objs := rtc_kern.o rtc_user.o
vfio_uml-objs := vfio_kern.o vfio_user.o

obj-y := stdio_console.o fd.o chan_kern.o chan_user.o line.o
obj-$(CONFIG_SSL) += ssl.o
obj-$(CONFIG_STDERR_CONSOLE) += stderr_console.o

obj-$(CONFIG_UML_NET_VECTOR) += vector.o
obj-$(CONFIG_UML_NET_VECTOR_V2) += vector2_cmdline.o vector2_core.o \
				   vector2_ethtool.o vector2_host_fd.o \
				   vector2_host_tap.o \
				   vector2_netdev.o vector2_runtime.o
obj-$(CONFIG_UML_NET_VECTOR_V2_CONFIG) += vector2_config.o
obj-$(CONFIG_UML_NET_VECTOR_V2_KUNIT) += vector2_config_test.o
obj-$(CONFIG_UML_NET_VECTOR_V2_QUEUE) += vector2_queue.o
obj-$(CONFIG_UML_NET_VECTOR_V2_QUEUE_KUNIT) += vector2_queue_test.o
obj-$(CONFIG_UML_NET_VECTOR_V2_TRANSPORT) += vector2_transport.o
obj-$(CONFIG_UML_NET_VECTOR_V2_TRANSPORT_KUNIT) += vector2_transport_test.o
obj-$(CONFIG_UML_NET_VECTOR_V2_FAKE_HOST) += vector2_fake_host.o
obj-$(CONFIG_UML_NET_VECTOR_V2_FAKE_HOST_KUNIT) += vector2_fake_host_test.o
obj-$(CONFIG_UML_NET_VECTOR_V2_MODEL) += vector2_model.o
obj-$(CONFIG_UML_NET_VECTOR_V2_MODEL_KUNIT) += vector2_model_test.o
obj-$(CONFIG_UML_NET_VECTOR_V2_CMDLINE_KUNIT) += vector2_cmdline_test.o
obj-$(CONFIG_UML_NET_VECTOR_V2_NETDEV_KUNIT) += vector2_netdev_test.o
obj-$(CONFIG_UML_NET_VECTOR_V2_ETHTOOL_KUNIT) += vector2_ethtool_test.o
obj-$(CONFIG_UML_NET_VECTOR_V2_HOST_FD_KUNIT) += vector2_host_fd_test.o
obj-$(CONFIG_UML_NET_VECTOR_V2_HOST_TAP_KUNIT) += vector2_host_tap_test.o
obj-$(CONFIG_MCONSOLE) += mconsole.o
obj-$(CONFIG_BLK_DEV_UBD) += ubd.o 
obj-$(CONFIG_UML_SOUND) += hostaudio.o
obj-$(CONFIG_NULL_CHAN) += null.o 
obj-$(CONFIG_PORT_CHAN) += port.o
obj-$(CONFIG_PTY_CHAN) += pty.o
obj-$(CONFIG_TTY_CHAN) += tty.o 
obj-$(CONFIG_XTERM_CHAN) += xterm.o xterm_kern.o
obj-$(CONFIG_UML_WATCHDOG) += harddog.o
obj-y += $(harddog-builtin-y) $(harddog-builtin-m)
obj-$(CONFIG_BLK_DEV_COW_COMMON) += cow_user.o
obj-$(CONFIG_UML_RANDOM) += random.o
obj-$(CONFIG_VIRTIO_UML) += virtio_uml.o
obj-$(CONFIG_UML_RTC) += rtc.o
obj-$(CONFIG_UML_PCI) += virt-pci.o
obj-$(CONFIG_UML_PCI_OVER_VIRTIO) += virtio_pcidev.o
obj-$(CONFIG_UML_PCI_OVER_VFIO) += vfio_uml.o

# pcap_user.o must be added explicitly.
USER_OBJS := fd.o null.o pty.o tty.o xterm.o vector_user.o
CFLAGS_null.o = -DDEV_NULL=$(DEV_NULL_PATH)

CFLAGS_xterm.o += '-DCONFIG_XTERM_CHAN_DEFAULT_EMULATOR="$(CONFIG_XTERM_CHAN_DEFAULT_EMULATOR)"'

include $(srctree)/arch/um/scripts/Makefile.rules
