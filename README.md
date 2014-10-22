Title: osdhud - a minimalist heads-up display for X11
Author: attila <attila@stalphonsos.com>
Date: 2014-10-16
Base Header Level: 2

# osdhud - minimalist heads-up display based on xosd

osdhud is a heads-up display (hud) for X windows.  It uses the xosd
library to draw its display over everything else and is designed to be
trivial to integrate into whatever desktop environment you use.  That
much said, I use
[cwm](https://en.wikipedia.org/wiki/Cwm_%28window_manager%29) under
[OpenBSD](http://www.openbsd.org) and find it meshes perfectly with
the minimalist desktop.

From the source code:

  The idea is that just running us from a keybinding in the window
  manager with no arguments should do something reasonable: the
  HUD appears for a couple of seconds and fades away if nothing else
  is done.  If we are invoked while the HUD is still up then it will
  stay up longer.  This is intuitively what I want:
      more hit key -> more hud
      stop hit key -> no more hud
  I call this the Caveman Theory of Human/Computer Interaction:
  PUNCH COMPUTER TO MAKE IT GO.

The first time osdhud is invoked it will daemonize itself and listen
on a Unix-domain socket; by default it lives in your home directory.
We do this so that osdhud can keep running statistics on things like
network utilization in the background.  Obviously these statistics get
better the longer osdhud runs.  If you want to start osdhud without
bringing up the display use the `-n` option; this can be useful in
your `~/.xinitrc` or similar script if you don't want the HUD to flash
for a few seconds when you start X windows.

## Building and Installing

Use the build.sh script in the top-level directory to invoke make.  It
either uses -f BSDmakefile or -f GNUmakefile depending on what the
native make command says.  Any arguments are passed directly to the `make`
invocation:

  $ ./build.sh
  The following targets are useful:
    help              produce this message
    all               build stuff
    install           install everything but desktop into $(PREFIX)
    clean             clean up temp files
    distclean         clean + reset to virgin state
    dist              cook dist-version.tar.gz tarball
  Install prefix: /usr/local (override with PREFIX=... on command-line)
    bin dir:  /usr/local/bin
    man dir:  /usr/local/man

The default installation prefix is `/usr/local`.  To change this
specify PREFIX=dir as an argument to build.sh, e.g.

  $ sudo ./build.sh PREFIX=/usr install

## Usage

Write me.
