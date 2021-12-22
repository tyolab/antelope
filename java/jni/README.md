
# Build for Android

## NDK Version
The library ca be built successfully with r10e and r18+ onward.

However, the latest NDK version brougt in "__register_atfork" function which is only available from Android 23. That means the libantelope_jni.so built by the latest NDK can only be loaded successfully in Android >23.


## Possible Build / Link Errors
~~~
java.lang.UnsatisfiedLinkError: dlopen failed: cannot locate symbol "__register_atfork" referenced by "libantelope.so" 
~~~

As discussed in this [thread](https://github.com/android-ndk/ndk/issues/692), from r16b to r17, libdl was included in libstd++, we have to explictly include -ldl in the make file for the linking

## 

```bash
(lldb) image lookup -vrn ATIRE_API_server::get_document
(lldb) settings show target.source-map
(lldb) settings set target.source-map /BuildDirectory/Sources/Clory Users/me/Sources/Clory
(lldb) set append target.source-map /buildbot/path /my/path
```

## check file content
```bash
readelf -s /home/dev/.lldb/module_cache/remote-android/.cache/F89E80B8/libantelope_jni.so | grep FILE
```
https://stackoverflow.com/questions/52986665/debugging-c-library-with-android-studio

## Finally
initially NDK17 was used for compiling with no success including symbol in the so file