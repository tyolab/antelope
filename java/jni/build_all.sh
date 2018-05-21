#!/bin/bash
cat Release.mk Common.mk > Application.mk

rm ../libs

./release.sh
