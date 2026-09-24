export PACKAGE_VERSION := 1.0.1

TARGET := iphone:clang:16.5:15.0

THEOS_PACKAGE_SCHEME = rootless

# arm64e has to come from Xcode's toolchain (SpringBoard is an arm64e process)
ARCHS = arm64 arm64e

include $(THEOS)/makefiles/common.mk

TWEAK_NAME = BootLogStop

BootLogStop_FILES = Tweak.x
BootLogStop_CFLAGS = -fobjc-arc -Wall

include $(THEOS_MAKE_PATH)/tweak.mk
