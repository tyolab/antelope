#!/bin/bash
cat Release.mk Common.mk > Application.mk
ndk-build
