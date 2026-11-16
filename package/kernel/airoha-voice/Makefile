# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=airoha-voice
PKG_RELEASE:=27

# 源码已内置为 vendor/airoha-voip-4222576d.tar.gz
#
# 原因：上游 commit 4222576d 已被 force push 从分支上丢弃。
# GitHub 目前还能按 SHA 取到 dangling commit，但随时可能被 GC 清理，
# 届时 CI 的 download 阶段会直接失败（第 6 次 CI 就是这样挂的）。
#
# 内置后：
#   - 不再依赖网络与上游仓库状态
#   - 构建可重复（哈希固定）
#   - 源码在 vendor/airoha-voip/ 有完整解压副本可供审阅
PKG_SOURCE:=airoha-voip-4222576d.tar.gz
PKG_SOURCE_URL:=file://$(TOPDIR)/vendor
PKG_HASH:=0ee5084a985ba0a3be96d5f232f86ad22c700ff99df6808ad97dac98f64ab97b
PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)-4222576d

PKG_LICENSE:=GPL-2.0-only AND LicenseRef-Proprietary
PKG_BUILD_PARALLEL:=1

include $(INCLUDE_DIR)/package.mk

define KernelPackage/airoha-en7581-pcm-spi
  SUBMENU:=Voice over IP
  TITLE:=Airoha EN7581 single-SLIC voice stack
  DEPENDS:=@TARGET_airoha_an7581_DEVICE_gemtek_xg2010g||TARGET_airoha_an7581_DEVICE_gemtek_xg2010g-ubi||TARGET_airoha_an7581_DEVICE_gemtek_xg2010g-2g-ubi
  FILES:= \
	$(PKG_BUILD_DIR)/src/en75xx-lec.ko \
	$(PKG_BUILD_DIR)/src/en75xx-pcm.ko \
	$(PKG_BUILD_DIR)/src/en75xx-voice.ko \
	$(PKG_BUILD_DIR)/src/en75xx-isi-spi.ko \
	$(PKG_BUILD_DIR)/src/en75xx-slic-si3219x.ko
  AUTOLOAD:=$(call AutoLoad,50,en75xx-lec en75xx-pcm en75xx-voice en75xx-isi-spi en75xx-slic-si3219x)
endef

define KernelPackage/airoha-en7581-pcm-spi/description
 Complete XG2010G EN7581 voice stack: PCM DMA, ISI transport, one Si32192
 ProSLIC FXS line, line control, hook/ring handling and /dev/en75xx-fxs0.
endef

define Package/airoha-voice-ctl
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=Telephony
  TITLE:=Airoha EN75xx FXS diagnostic utility
  DEPENDS:=+kmod-airoha-en7581-pcm-spi
endef

define Package/airoha-voice-ctl/description
 Inspect and control EN75xx FXS line state through the stable voice UAPI.
 Read-only commands are suitable for bring-up; ringing and tone generation
 require an explicit command.
endef

define KernelPackage/airoha-en7581-pcm-spi/install
	$(INSTALL_DIR) $(1)/lib/firmware/en75xx/proslic
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/firmware/proslic/si3219x_a_lcqc.fw \
		$(1)/lib/firmware/en75xx/proslic/
endef

define Build/Compile
	+$(KERNEL_MAKE) $(PKG_JOBS) M="$(PKG_BUILD_DIR)/src" modules
	$(TARGET_CC) $(TARGET_CPPFLAGS) $(TARGET_CFLAGS) \
		-Wall -Wextra -Werror \
		-I$(PKG_BUILD_DIR)/include/uapi \
		-o $(PKG_BUILD_DIR)/airoha-voice-ctl \
		$(CURDIR)/files/airoha-voice-ctl.c \
		$(TARGET_LDFLAGS)
endef

define Package/airoha-voice-ctl/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/airoha-voice-ctl $(1)/usr/sbin/
endef

$(eval $(call KernelPackage,airoha-en7581-pcm-spi))
$(eval $(call BuildPackage,airoha-voice-ctl))
