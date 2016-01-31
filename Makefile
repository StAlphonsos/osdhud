# -*- makefile -*-
# the text between ##+ and ##- is spit out by the "help" target:
##+
# The following targets are useful:
#     help              produce this message
#     all               build stuff
#     install           install everything but desktop into $(PREFIX)
#     clean             clean up temp files
#     distclean         clean + reset to virgin state
#     dist              cook dist-version.tar.gz tarball
##-

PACKAGE_NAME=osdhud
DIST_VERS!=cat VERSION
S!=pwd

.if exists(config.mk)
.  include "config.mk"
.endif
.include "settings.mk"

BINARIES=osdhud

MAKESYS?=Makefile settings.mk suss.pl configure
SUDIRS?=web
MANSECT?=1
MANEXT?=$(MANSECT)
## Can be set per-OS
MANSRC?=osdhud.mandoc
MANPAGE?=osdhud.$(MANEXT)
DOCS?=$(MANSRC)
FILES?=osdhud.c openbsd.c osdhud.h movavg.c movavg.h $(DOCS)
DIST_NAME?=$(PACKAGE_NAME)
DIST_TMP?=$(DIST_NAME)-$(DIST_VERS)
DIST_LIST?=VERSION *.md *.in $(MAKESYS) $(SUBDIRS) $(FILES)
DIST_TAR?=$(DIST_NAME)-$(DIST_VERS).tar
DIST_TAR_GZ?=$(DIST_TAR).gz
DOC_EPHEM?=README.aux README.glo README.idx README.ist README.log README.out README.tex README.pdf README.toc $(MANPAGE)

default: all

help::
	@$(SED) -e '1,/^##+/d' -e '/^##-/,$$d' -e 's/^# //' < Makefile
	@$(ECHO) Install prefix: $(PREFIX) '(override with PREFIX=... on command-line)'
	@$(ECHO) '    bin dir: ' $(BINDIR)
	@$(ECHO) '    man dir: ' $(MANDIR)

all:: $(BINARIES) man-page

osdhud: osdhud.o movavg.o $(UNAME).o
	$(CC) $(LDFLAGS) -o $@ osdhud.o movavg.o $(UNAME).o $(LIBS)

## My thinking here is that I'm just going to go with OpenBSD mandoc
## since osdhud is so far only really usable under OpenBSD.  I would
## like to explore writing manuals in multimarkdown and producing
## mandoc output but it doesn't seem like the tools that exist right
## now are that great there... a lot of mandoc features are not
## accessible this way, or maybe I'm missing something.
##
## osdhud.mandoc therefore is just a straight mandoc source file,
## unashamedly OpenBSD-specific.  I'm writing that way to get
## experience with how to use mandoc well.  Since I want @VERSION@
## expanded in the final osdhud.1 I'll use my Perl keyword expander to
## do that.  If I have to write man pages for other OSes then I'll try
## doing something in mmd and translating to nroff -man.  I could then
## take the opportunity to see how hard it would be to go the extra
## mile to mandoc that is equivalent to the hand-written mandoc file.

MANIFY?=$(SUSS) VERSION=$(VERSION)

man-page:: $(MANPAGE)

$(MANPAGE):: $(MANSRC) VERSION
	$(MANIFY) < $(MANSRC) > $(MANPAGE)

WEBFILES=web/osdhud.html web/osdhud.pdf

web: $(WEBFILES)

web/osdhud.html: osdhud.1
	$(MANDOC) -T html osdhud.1 > $@

web/osdhud.pdf: osdhud.1
	$(MANDOC) -T pdf osdhud.1 > $@

osdhud.o: osdhud.c osdhud.h movavg.h config.h version.h
movavg.o: movavg.h

# config.h doesn't need to be regenerated normally
version.h: version.h.in VERSION
	$(SUSS) -file=version.h VERSION=$(VERSION)

$(UNAME).o: $(UNAME).c osdhud.h movavg.h

.c.o:
	$(CC) -c $(CFLAGS) -o $@ $<

install:: all
	$(INSTALL_X) $(BINARIES) $(BINDIR)
	$(INSTALL) $(MANPAGE) $(MANDIR)/man$(MANEXT)/

clean::
	$(RM) -f osdhud.o movavg.o $(UNAME).o osdhud version.h $(DOC_EPHEM)

distclean:: clean
	$(RM) -f $(DIST_TAR) $(DIST_TAR_GZ) config.h config.mk
	$(RM) -rf $(DIST_TMP)

dist: distclean $(DIST_TAR_GZ)

checkdist: $(DIST_TMP)
	(cd $(DIST_TMP); ./configure && $(MAKE) $(MFLAGS) && ./osdhud -h && $(MAKE) $(MFLAGS) distclean)

$(DIST_TAR): $(DIST_TMP)
	$(TAR_CF) $(DIST_TAR) $(DIST_TMP)

$(DIST_TAR_GZ): $(DIST_TAR)
	$(GZIP) $(DIST_TAR)

$(DIST_TMP):
	$(MKDIR) -p $(DIST_TMP)
	($(TAR_CF) - $(DIST_LIST)) | (cd $(DIST_TMP); $(TAR_XF) -)
