ARCHS = arm64 arm64e
TARGET = iphone:clang:16.5:15.0
# 默认 roothide；rootless 包用变量覆盖：THEOS_PACKAGE_SCHEME=rootless
THEOS_PACKAGE_SCHEME ?= roothide

include $(THEOS)/makefiles/common.mk

TWEAK_NAME = SenseDark SenseDarkSettings
SenseDark_FILES = Tweak.x
SenseDark_CFLAGS = -fobjc-arc

SenseDarkSettings_FILES = SenseDarkSettings.x
SenseDarkSettings_CFLAGS = -fobjc-arc
include $(THEOS_MAKE_PATH)/tweak.mk
