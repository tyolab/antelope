#APP_OPTIM := release
APP_OPTIM := debug
APP_CFLAGS := -DDEBUG -g
APP_CPPFLAGS += -frtti # -fexceptions

APP_STL := stlport_static
APP_ABI := armeabi

APP_PLATFORM := android-8

LOCAL_ARM_MODE := thumb
