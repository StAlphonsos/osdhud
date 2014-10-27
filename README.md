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

## Building and Installing

Use the make.sh script in the top-level directory to invoke make.  It
either uses -f BSDmakefile or -f GNUmakefile depending on what the
native make command says.  Any arguments are passed directly to the
`make` invocation:

  $ ./make.sh
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

  # Linux
  $ sudo ./make.sh PREFIX=/usr install
  
  # To install in ~/bin
  $ ./make.sh PREFIX=$HOME install

## Usage

The first time osdhud is invoked it will daemonize itself and listen
on a Unix-domain socket; by default it lives in your home directory.
We do this so that osdhud can keep running statistics on things like
network utilization in the background.  Obviously these statistics get
better the longer osdhud runs.  If you want to start osdhud without
bringing up the display use the `-n` option; this can be useful in
your `~/.xinitrc` or similar script if you don't want the HUD to flash
for a few seconds when you start X windows.

## TO-DO

* Better network interface specifications:
  all   - all interfaces that are up except for lo*
  alll  - all + loopback(s)
  group - e.g. egress
  a,b,c - multiple interfaces separated by commas
  em\d+ - all interfaces matching regexp em\d+
* Fix issues with sampling interval changing.
  Maybe just ditch that, perhaps it doesn't make a difference
* Query network interfaces for their maximum speed.  This is highly
  OS-specific.  Under OpenBSD I believe this can only be gleaned from
  the media flags (IFM_xxx) that you get back from one of those damned
  ioctl's or routing socket messages...  We should map those into
  a table lookup or something.  Given the maximum speed we can then
  display a percentage meter underneath the text.
* xosd: our display is not always on top.  Why?  Isn't that what
  their update thread is for?
