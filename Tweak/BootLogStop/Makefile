# 版本號只寫在這裡:也是 tweak 和設定 bundle 的 BLS_VERSION(「關於」的版本列、trace 開頭),
# bundle/Info.plist 的 __VERSION__ 則在 stage 時填入。control 的 Version 由 Theos 用這個值覆蓋。
export PACKAGE_VERSION := 1.1.0

TARGET := iphone:clang:16.5:15.0

THEOS_PACKAGE_SCHEME = rootless

# arm64e slice 由 Xcode toolchain 產生(同 NowLyrics 的說明),SpringBoard 是 arm64e 程序。
ARCHS = arm64 arm64e

include $(THEOS)/makefiles/common.mk

TWEAK_NAME += BootLogStop

BootLogStop_FILES += Tweak.x
BootLogStop_CFLAGS += -fobjc-arc -Wall -DBLS_VERSION=\"$(PACKAGE_VERSION)\"

include $(THEOS_MAKE_PATH)/tweak.mk

# 設定頁:prefs/ 裡是 BootLogStop.plist(entry + items)和各語言的 Localizable.strings;
# BootLogStopPrefs.bundle 是真的 preference bundle,主類別 BLSRootListController 繼承 libprefs 的
# PLLocalizedListController,加上 SF Symbol 圖示、「上次開機」三列和右上角「套用」(respring)按鈕。
# libprefs 由 <libprefs/prefs.h> 的 module map 自動連結(@rpath,Theos 的 rootless rpath 指到 /var/jb/usr/lib)。
BUNDLE_NAME += BootLogStopPrefs

BootLogStopPrefs_FILES += prefsbundle/BLSRootListController.m
BootLogStopPrefs_CFLAGS += -fobjc-arc -Wall -DBLS_VERSION=\"$(PACKAGE_VERSION)\"
BootLogStopPrefs_FRAMEWORKS += UIKit
BootLogStopPrefs_PRIVATE_FRAMEWORKS += Preferences
BootLogStopPrefs_INSTALL_PATH = /Library/PreferenceBundles
BootLogStopPrefs_RESOURCE_DIRS = bundle

include $(THEOS_MAKE_PATH)/bundle.mk

# PreferenceLoader 會遞迴掃描 Preferences/ 底下所有 .plist,plist 所在的資料夾就是它的
# 來源 bundle,PLLocalizedListController 從那裡讀各語言的 Localizable.strings。
internal-stage::
	$(ECHO_NOTHING)mkdir -p "$(THEOS_STAGING_DIR)/Library/PreferenceLoader/Preferences/BootLogStop"$(ECHO_END)
	$(ECHO_NOTHING)cp -R prefs/ "$(THEOS_STAGING_DIR)/Library/PreferenceLoader/Preferences/BootLogStop/"$(ECHO_END)
	$(ECHO_NOTHING)sed "s/__VERSION__/$(PACKAGE_VERSION)/g" bundle/Info.plist > "$(THEOS_STAGING_DIR)/Library/PreferenceBundles/BootLogStopPrefs.bundle/Info.plist"$(ECHO_END)
