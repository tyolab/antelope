
# Build for Android

## NDK Version
The library ca be built successfully with r10e and r18+ onward.

However, the latest NDK version brougt in "__register_atfork" function which is only available from Android 23. That means the libantelope_jni.so built by the latest NDK can only be loaded successfully in Android >23.


## Possible Build / Link Errors
~~~
java.lang.UnsatisfiedLinkError: dlopen failed: cannot locate symbol "__register_atfork" referenced by "libantelope.so" 
~~~

As discussed in this [thread](https://github.com/android-ndk/ndk/issues/692), from r16b to r17, libdl was included in libstd++, we have to explictly include -ldl in the make file for the linking