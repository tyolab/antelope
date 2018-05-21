APP_OPTIM := debug
APP_CFLAGS := -DDEBUG -g

APP_ABI := armeabi-v7a arm64-v8a  x86

APP_CPPFLAGS += -frtti -fexceptions

##
## Options:
## * stlport_static
## * c++_shared
## * system
## 
APP_STL := c++_shared

APP_PLATFORM := android-27
