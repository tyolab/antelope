#APP_OPTIM := release
APP_OPTIM := debug
APP_CFLAGS := -DDEBUG -g
APP_CPPFLAGS += -frtti # -fexceptions

ifeq ($(NO_NEON),)
APP_ABI := armeabi-v7a
else ifneq ($(TEGRA2),)
APP_ABI := armeabi-v7a
else
APP_ABI := armeabi
endif

APP_STL := stlport_static
APP_ABI := armeabi

APP_PLATFORM := android-10

LOCAL_ARM_MODE := thumb
