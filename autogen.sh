#!/bin/bash

libtool=

case `uname` in 
    Darwin*) 
        libtool=glibtoolize 
        ;;
    *) 
        libtool=libtoolize 
        ;; 
esac

aclocal && $libtool --copy && automake --add-missing --copy --force-missing && autoconf
