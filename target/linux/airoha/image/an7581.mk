define Build/an7581-emmc-bl2-bl31-uboot
  head -c $$((0x800)) /dev/zero > $@
  cat $(STAGING_DIR_IMAGE)/an7581_$1-bl2.fip >> $@
  dd if=$(STAGING_DIR_IMAGE)/an7581_$1-bl31-u-boot.fip of=$@ bs=1 seek=$$((0x20000)) conv=notrunc
endef

define Build/an7581-preloader
  $(STAGING_DIR_HOST)/bin/fiptool create \
		--tb-fw $(STAGING_DIR_IMAGE)/an7581-bl2.bin \
		$(STAGING_DIR_IMAGE)/an7581_$1-bl2.fip
  cat $(STAGING_DIR_IMAGE)/an7581_$1-bl2.fip >> $@
endef

define Build/an7581-bl31-uboot
  $(STAGING_DIR_HOST)/bin/fiptool create \
		--soc-fw $(STAGING_DIR_IMAGE)/an7581-bl31.lzma \
		--nt-fw $(STAGING_DIR_IMAGE)/an7581_$1-u-boot.lzma \
		$(STAGING_DIR_IMAGE)/an7581_$1-bl31-u-boot.fip
  cat $(STAGING_DIR_IMAGE)/an7581_$1-bl31-u-boot.fip >> $@
endef

define Build/an7581-chainloader
  $(INSTALL_DIR) $(KDIR)/chainload-fit-$(notdir $@)
  @if [ -f "$(STAGING_DIR_IMAGE)/an7581_$1-u-boot.lzma" ]; then \
    KERNEL="$(STAGING_DIR_IMAGE)/an7581_$1-u-boot.lzma"; \
    COMP="lzma"; \
  else \
    KERNEL="$(STAGING_DIR_IMAGE)/an7581_$1-u-boot.bin"; \
    COMP="none"; \
  fi; \
  $(TOPDIR)/scripts/mkits.sh \
    -D $(DEVICE_NAME) \
    -o $(KDIR)/chainload-fit-$(notdir $@)/u-boot.its \
    -k $$KERNEL \
    -C $$COMP \
    -a 0x80200000 -e 0x80200000 \
    -c conf-uboot \
    -A arm64 -v u-boot \
    -d $(STAGING_DIR_IMAGE)/an7581_$1-u-boot.dtb \
    -s 0x82000000
  PATH=$(LINUX_DIR)/scripts/dtc:$(PATH) \
    $(STAGING_DIR_HOST)/bin/mkimage \
    -D "-i $(KDIR)/chainload-fit-$(notdir $@)" \
    -f $(KDIR)/chainload-fit-$(notdir $@)/u-boot.its \
    $(STAGING_DIR_IMAGE)/an7581_$1-chainload-u-boot.itb
  cat $(STAGING_DIR_IMAGE)/an7581_$1-chainload-u-boot.itb >> $@
endef

define Device/FitImageLzma
	KERNEL_SUFFIX := -uImage.itb
	KERNEL = kernel-bin | lzma | fit lzma $$(KDIR)/image-$$(DEVICE_DTS).dtb
	KERNEL_NAME := Image
endef












define Device/gemtek_xg2010g-common
  DEVICE_VENDOR := Gemtek
  DEVICE_PACKAGES := fitblk kmod-leds-gpio kmod-gpio-button-hotplug \
	kmod-phy-airoha-en8811h kmod-phy-realtek rtl826x-firmware \
	kmod-nf-conntrack-bridge kmod-nft-bridge \
	kmod-airoha-en7572 kmod-airoha-xpon airoha-ponctl airoha-pond \
	kmod-airoha-tod kmod-airoha-en7581-pcm-spi \
	-airoha-an7581-mt7996-board -airoha-en7581-mt7996-npu-firmware \
	-kmod-mac80211 -kmod-mt7996-firmware -kmod-mt7996e \
	-wpad-mbedtls -wpad-mesh-mbedtls -wireless-regdb
  BLOCKSIZE := 128k
  PAGESIZE := 2048
  UBINIZE_OPTS := -E 5
  KERNEL_IN_UBI := 1
  KERNEL := kernel-bin | gzip
  IMAGES := sysupgrade.itb
  # The dynamic UBI fit volume is recreated to the uploaded image size.
  # The 128 MiB package limit is independent of UBI volume placement.
  IMAGE_SIZE := 131072k
  IMAGE/sysupgrade.itb := append-kernel | \
	fit gzip $$(KDIR)/image-$$(firstword $$(DEVICE_DTS)).dtb external-static-with-rootfs | \
	append-metadata | check-size
  SOC := an7581
endef

define Device/gemtek_xg2010g-ubi-common
  # Only the UBI layout keeps everything but the preloader inside the UBI
  # volumes, so the 2.0 compat version and its warning belong to the -ubi
  # variants rather than to the vendor layout of Device/gemtek_xg2010g.
  DEVICE_VARIANT := UBI
  DEVICE_COMPAT_VERSION := 2.0
  DEVICE_COMPAT_MESSAGE := Firmware requires the XG2010G OpenWrt U-Boot UBI layout \
       with bl2 at 0x00000000 and ubi starting at 0x00020000, containing \
       fip, ubootenv, ubootenv2, factory and fit. Do not force an upgrade from \
       the legacy 0x00600000 layout. Preserve bl2 and the boot/calibration volumes.
endef

define Device/gemtek_xg2010g
  # DEVICE_DTS must be set before the common block: the common IMAGE line
  # uses := and freezes $(firstword $(DEVICE_DTS)) at eval time.
  DEVICE_DTS := an7581-xg2010g
  $(call Device/gemtek_xg2010g-common)
  DEVICE_MODEL := XG2010G
  SUPPORTED_DEVICES := gemtek,xg2010g
endef
TARGET_DEVICES += gemtek_xg2010g

define Device/gemtek_xg2010g-ubi
  # DEVICE_DTS must be set before the common block: the common IMAGE line
  # uses := and freezes $(firstword $(DEVICE_DTS)) at eval time.
  DEVICE_DTS := an7581-gemtek-xg2010g-ubi
  $(call Device/gemtek_xg2010g-common)
  $(call Device/gemtek_xg2010g-ubi-common)
  DEVICE_MODEL := XG2010G
  # One image covers the 1 GiB and 2 GiB boards: U-Boot publishes the DRAM
  # size detected by BL2, which overrides the fallback in the device tree.
  # The 2 GiB compatibles are kept so upgrades from the old variant work.
  SUPPORTED_DEVICES := gemtek,xg2010g-ubi gemtek,xg2010g \
	gemtek,xg2010g-2g-ubi gemtek,xg2010g-2g
endef
TARGET_DEVICES += gemtek_xg2010g-ubi

