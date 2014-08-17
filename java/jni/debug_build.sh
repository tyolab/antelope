#!/bin/bash
cat Debug.mk Common.mk > Application.mk
ndk-build NDK_DEBUG=1 NDK_LOG=1
