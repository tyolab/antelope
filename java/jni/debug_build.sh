#!/bin/bash
cat Debug.mk Common.mk > Application.mk

rm ../libs

./debug.sh
