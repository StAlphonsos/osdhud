# osdhud - minimalist heads-up display based on xosd

*N.B.*: Please read [the wiki node](http://traq.haqistan.net/wiki/osdhud) for the most up-to-date information including links to source tarballs

osdhud is a heads-up display (hud) for X windows.  It uses the xosd
library to draw its display over everything else and is designed to be
trivial to integrate into whatever desktop environment you use.  That
much said, I use
[cwm](https://en.wikipedia.org/wiki/Cwm_%28window_manager%29) under
[OpenBSD](http://www.openbsd.org) and find it meshes perfectly with
the minimalist desktop: no fixed screen real-estate dedicated to
gauges, widgets or crud.  If you want to know what's going on in your
machine, hit a key.  If you want to know more there's always
xterm -e systat :-)

At the moment it only is purported to work under OpenBSD.  FreeBSD
might still work but I have not sat in front of a FreeBSD machine
in a few months.  Linux is unsupported at the moment but should be
relatively easy to do if anyone cares.

## Administrivia

I adhere loosely to the (git-flow)[http://nvie.com/posts/a-successful-git-branching-model/] concept in my development.  All of the real action happens
in the branch named `develop`.  The `master` branch should only have
releases on it, in the form of merges from `develop` and tags.  I don't
actually use the (supposedly very nice) `git-flow` tool/extension
in my work.  The repository on github are synced more or less as I do
things but the one on (traqistan)[http://trac.haqistan.net/browser/osdhud]
is always the most up-to-date.

Please feel free to (contact me)[http://trac.haqistan.net/~attila]
if you want to collaborate.

## Building and Installing

Use the make.sh script in the top-level directory to invoke make.  It
either uses -f BSDmakefile or -f GNUmakefile depending on what the
native make command says.  Any arguments are passed directly to the
`make` invocation:

```
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
```

The default installation prefix is `/usr/local`.  To change this
specify PREFIX=dir as an argument to build.sh, e.g.

```
  # Linux (if anyone cares enough for me to port it :-)
  $ sudo ./make.sh PREFIX=/usr install
  
  # To install in ~/bin
  $ ./make.sh PREFIX=$HOME install
```

*N.B.* It doesn't work under Linux, the above was just an example.

## Usage

From the source code (again, please see [the wiki node](http://traq.haqistan.net/wiki/osdhud) for more complete information):

```
  The idea is that just running us from a keybinding in the window
  manager with no arguments should do something reasonable: the
  HUD appears for a couple of seconds and fades away if nothing else
  is done.  If we are invoked while the HUD is still up then it will
  stay up longer.  This is intuitively what I want:
      more hit key -> more hud
      stop hit key -> no more hud
  I call this the Caveman Theory of Human/Computer Interaction:
  PUNCH COMPUTER TO MAKE IT GO.
```

The first time osdhud is invoked it will daemonize itself and listen
on a Unix-domain socket; by default it lives in your home directory.
We do this so that osdhud can keep running statistics on things like
network utilization in the background.  Obviously these statistics get
better the longer osdhud runs (although at the moment I don't do too
much of that).  If you want to start osdhud without bringing up the
display use the `-n` option; this can be useful in your `~/.xinitrc`
or similar script if you don't want the HUD to flash for a few seconds
when you start X windows.

There is still not a man page, but that's high on the list.
