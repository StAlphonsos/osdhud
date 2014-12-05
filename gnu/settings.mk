# -*- makefile -*-

include generic/settings.mk

VERSION=$(shell cat $(S)/VERSION)
UNAME=$(shell uname | tr A-Z a-z)
XLSD_LIBS=$(shell xosd-config --libs)
XOSD_CFLAGS=$(shell xosd-config --cflags)
JUDY_LIBS=-lJudy
C_DEBUGGING?=-g -ggdb -Wall -Werror
CFLAGS+=$(C_DEBUGGING) -I/usr/local/include
LDFLAGS+=-L/usr/local/lib
LIBS+=$(XOSD_LIBS) $(JUDY_LIBS) $(PTHREAD_LIBS)
