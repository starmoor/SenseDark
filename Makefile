ARCHS = arm64 arm64e
TARGET = iphone:clang:16.5:15.0
THEOS_PACKAGE_SCHEME ?= roothide

include $(THEOS)/makefiles/common.mk

TWEAK_NAME = SenseDark SenseDarkSettings
SenseDark_FILES = Tweak.x
SenseDark_CFLAGS = -fobjc-arc -Os
SenseDarkSettings_FILES = SenseDarkSettings.x
SenseDarkSettings_CFLAGS = -fobjc-arc -Os

BUNDLE_NAME = SenseDarkPrefs
SenseDarkPrefs_FILES = SenseDarkPrefs.m
SenseDarkPrefs_CFLAGS = -fobjc-arc -Os
SenseDarkPrefs_INSTALL_PATH = /Library/PreferenceBundles
SenseDarkPrefs_PRIVATE_FRAMEWORKS = Preferences

include $(THEOS_MAKE_PATH)/tweak.mk
include $(THEOS_MAKE_PATH)/bundle.mk

# theos 的 Signing 步骤在本机产出无效签名（flags=0x0/.unsigned），AMFI 拒载
# —— 打包前用 PATH 里的 ldid 强制重签全部 Mach-O
#
# 双 scheme 各自独立包 ID + 名称后缀：flat 源里同一 ID 双架构条目会让 Zebra
# 取错条目（Unsupported Architecture），分 ID 后客户端按架构静默过滤，单源共存
before-package::
	find "$(THEOS_STAGING_DIR)" -type f \( -name "*.dylib" -o -name "SenseDarkPrefs" \) -exec ldid -S {} +
	sed -e 's/^Package: com\.starmoor\.sensedark$$/Package: com.starmoor.sensedark.$(THEOS_PACKAGE_SCHEME)/' \
	    -e 's/^Name: SenseDark$$/Name: SenseDark ($(THEOS_PACKAGE_SCHEME))/' \
	    "$(THEOS_STAGING_DIR)/DEBIAN/control" > "$(THEOS_STAGING_DIR)/DEBIAN/control.tmp" \
	  && mv "$(THEOS_STAGING_DIR)/DEBIAN/control.tmp" "$(THEOS_STAGING_DIR)/DEBIAN/control"

after-install::
	install.exec "killall -9 SpringBoard || true"
