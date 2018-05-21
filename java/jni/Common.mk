
APP_CPPFLAGS += -frtti -fexceptions

##
## Options:
## * stlport_static
## * c++_shared
## * system
## 
APP_STL := c++_shared

APP_PLATFORM := android-27

#LOCAL_ARM_MODE := thumb
ifeq ($(TARGET_ARCH),arm)
    LOCAL_CFLAGS := -mfpu=neon -march=armv6t2 -O9
    LOCAL_SRC_FILES := engine-arm.s
endif
ifeq ($(TARGET_ARCH),x86)
    LOCAL_CFLAGS := -msse2 -m32 -masm=intel
    LOCAL_SRC_FILES := engine-x86.s
endif