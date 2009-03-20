#!/bin/sh

set -ex

SDK=/Developer/SDKs/MacOSX10.5.sdk/usr/include
rm -rf usr_include
rsync -a ${SDK}/i386 \
         ${SDK}/architecture \
         ${SDK}/libkern \
         ${SDK}/mach \
         ${SDK}/mach-o \
         ${SDK}/mach_debug \
         ${SDK}/machine \
         ${SDK}/sys/_types.h \
         usr_include/
patch -p0 < usr_include.patch
find usr_include -name '*.h' -type f \
    -exec sed -i '' -e 's/__unused/__attribute__((__unused__))/g' {} \;
cat > usr_include/libc.h << __EOF__
#ifndef _LIBC_H
#define _LIBC_H

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/param.h>

#endif
__EOF__
tar -jcf usr_include.tar.bz2 usr_include
