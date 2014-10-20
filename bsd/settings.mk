# -*- makefile -*-
##
# osdhud settings on BSD systems
##

.include "$(S)/generic/settings.mk"

VERSION!=cat $(S)/VERSION

UNAME!=uname | tr A-Z a-z

.if "${UNAME}" == "openbsd"
PTHREAD_CFLAGS=
PTHREAD_LDFLAGS=
PTHREAD_LIBS=-lpthread
.elif "${UNAME}" == "freebsd"  
PTHREAD_CFLAGS!=pthread-config --cflags
PTHREAD_LDFLAGS!=pthread-config --ldflags
PTHREAD_LIBS!=pthread-config --libs
.endif

## N.B. xosd-config --cflags output is obnoxious, at least on my box:
##   $ xosd-config --cflags
##   -O2 -pipe -fno-strict-aliasing -std=gnu89 -I/usr/local/include -Wall
## They should not be deciding about -Wall, -std etc for me.   Irritating.
## I therefore ignore XOSD_CFLAGS.

XOSD_LIBS!=xosd-config --libs
XOSD_CFLAGS=-I/usr/local/include -I/usr/X11R6/include

C_WARNINGS?=-Wall -Werror
C_DEBUGGING?=-g -ggdb $(C_WARNINGS)
C_OPTIMIZED?=-O2 -pipe $(C_WARNINGS)
#CFLAGS=$(C_DEBUGGING) $(XOSD_CFLAGS) $(PTHREAD_CFLAGS)
CFLAGS=$(C_DEBUGGING) $(XOSD_CFLAGS)
LDFLAGS+=$(XOSD_LDFLAGS)
LIBS+=$(XOSD_LIBS) $(PTHREAD_LIBS)
#LIBS+=$(XOSD_LIBS)
