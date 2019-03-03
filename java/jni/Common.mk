
APP_CPPFLAGS += -frtti -fexceptions

##
## Options:
## * stlport_static
## * c++_shared / c++_static
## * system
##
## Note: the latest NDKs introduce lots of new functions which were not
## included in the system libraries of old Androids. In that case, using
## c++_static would be a better idea
## 
APP_STL := c++_static

##
## After android-21, new functions are used and they don't exist on old Android platforms
## 
APP_PLATFORM := android-20

#LOCAL_ARM_MODE := thumb
ifeq ($(TARGET_ARCH),arm)
    LOCAL_CFLAGS := -mfpu=neon -march=armv6t2 -O9
    LOCAL_SRC_FILES := engine-arm.s
endif
ifeq ($(TARGET_ARCH),x86)
    LOCAL_CFLAGS := -msse2 -m32 -masm=intel
    LOCAL_SRC_FILES := engine-x86.s
endif