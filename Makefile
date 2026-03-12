ARCHS = arm64 arm64e
TARGET = iphone:clang:16.5:15.0
THEOS_PACKAGE_SCHEME ?= roothide

include $(THEOS)/makefiles/common.mk

TWEAK_NAME = SenseDark SenseDarkSettings
SenseDark_FILES = Tweak.x
SenseDark_CFLAGS = -fobjc-arc
SenseDarkSettings_FILES = SenseDarkSettings.x
SenseDarkSettings_CFLAGS = -fobjc-arc

BUNDLE_NAME = SenseDarkPrefs
SenseDarkPrefs_FILES = SenseDarkPrefs.m
SenseDarkPrefs_CFLAGS = -fobjc-arc
SenseDarkPrefs_INSTALL_PATH = /Library/PreferenceBundles
SenseDarkPrefs_PRIVATE_FRAMEWORKS = Preferences

include $(THEOS_MAKE_PATH)/tweak.mk
include $(THEOS_MAKE_PATH)/bundle.mk

# theos 的 Signing 步骤在本机产出无效签名（flags=0x0/.unsigned），AMFI 拒载
# —— 打包前用 PATH 里的 ldid 强制重签全部 Mach-O
before-package::
	find "$(THEOS_STAGING_DIR)" -type f \( -name "*.dylib" -o -name "SenseDarkPrefs" \) -exec ldid -S {} +

after-install::
	install.exec "killall -9 SpringBoard || true"
