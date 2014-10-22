# -*- makefile -*-

include generic/settings.mk

VERSION=$(shell cat $(S)/VERSION)
UNAME=$(shell uname | tr A-Z a-z)

C_DEBUGGING?=-g -ggdb -Wall -Werror
CFLAGS=$(C_DEBUGGING) -I/usr/local/include
LDFLAGS+=-L/usr/local/lib
LIBS+=-lxosd
