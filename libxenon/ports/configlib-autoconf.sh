#!/bin/bash

# 
# Simple script for porting almost all autoconf-based libraries & tools.
# Written by Fabrice Guilhaume <sfx.gligli@free.fr>, public domain.
#

./configure --prefix=$DEVKITXENON/usr --host=ppc-elf CC=powerpc-elf-gcc LD=powerpc-elf-ld \
        LDFLAGS="-L$DEVKITXENON/usr/lib/ -L$DEVKITXENON/xenon/lib/32/ -lxenon -T$DEVKITXENON/app.lds -u read -u _start -u exc_base" \
        CFLAGS="-mcpu=cell -mtune=cell -m32 -fno-pic -mpowerpc64 -I$DEVKITXENON/usr/include/"
