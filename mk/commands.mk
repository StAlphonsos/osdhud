# -*- makefile -*-
##
# BSD command defaults
##

INSTALL?=install -vC
INSTALL_D?=install -d
MANDOC?=mandoc
ECHO?=echo
SED?=sed
CAT?=cat
CP?=cp
LN?=ln
RM=rm
TAR?=tar
TAR_CF?=$(TAR) cf
TAR_XF?=$(TAR) xf
GZIP?=gzip
MKDIR?=mkdir
MKDIR_P?=$(MKDIR) -p
SUSS?=./suss.pl
INSTALL?=install
INSTALL_X?=$(INSTALL)
INSTALL_D?=$(INSTALL) -d
