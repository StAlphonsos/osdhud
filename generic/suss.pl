#!/usr/bin/perl
##
# suss.pl - s/@tokens@/$actual_values_or_empty_string/
#
# Coyright (C) 1999-2007 by attila <attila@stalphonsos.com>
# All Rights Reserved.
#
# See the POD at EOF for docs, or invoke with -help -verbose
##
use strict;
use warnings;
use Pod::Usage;
use IO::Handle;
use IO::File;
use vars qw($P $COPY_YEARS $VERBOSE $DEFAULTS $VERSION);

BEGIN {
    ($P) = reverse(split('/', $0)); # XXX File::Spec would be better
    my $yyyy = 1900+(localtime(time))[5];
    $COPY_YEARS = sprintf(($yyyy == 2007) ? q{%d} : q{%d-%d}, 2007, $yyyy);
    $VERBOSE = 0;
    $DEFAULTS = {
    };
    $VERSION = '0.1.0';
}

## qchomp - trim leading and trailing whitespace and deal with quoted strings
##
sub qchomp {
    my $str = shift(@_);
    while ($str =~ /^\s*([\"\'])(.*)\1\s*$/) {
        $str = $2;
    }
    $str =~ s/^\s+//;
    $str =~ s/\s+$//;
    return $str;
}

## usage - dump a usage message and die
##
sub usage {
    my($msg) = @_;
    pod2usage(-verbose => 2)            if $::VERBOSE && !defined($msg);
    if (defined($msg)) {
        print STDERR "$::P: $msg\n"     if defined $msg;
    } else {
        print STDERR "$::P: s/\@tokens\@/\$values/g from the command line\n";
    }
    print STDERR "usage: $::P [-options] [args]\n";
    print STDERR "       Standard options:\n";
    print STDERR "          -v|verbose      increment verbosity level\n";
    print STDERR "          -V|verbosity=n  set verbosity level to n\n\n";
    print STDERR "          -help           print this brief message\n";
    print STDERR "       To see the full documentation, try:\n\n";
    print STDERR "           \$ $::P -help -verbose\n";
    exit(defined($msg)? 1:0);
}

## parse_argv - simplistic and effective CLA parser
##
sub parse_argv {
    my $args;
    if (@_ && (ref($_[0]) eq 'HASH')) {
        $args = shift(@_);
    } else {
        $args = {};
    }
    my @argv = @_;
    foreach my $arg (@argv) {
        $arg =~ s/^\s+//;
        $arg =~ s/\s+$//;
        next unless length $arg;
        if ($arg =~ /^(-{1,2}[^=]+?)[=](.*)$/) {
            my($k,$v) = ($1,qchomp($2));
            $k =~ s/^-+//;
            if ($k ne '_') {
                if (!exists($args->{$k}) ||
                    (ref($args->{$k}) !~ /^(ARRAY|HASH)$/)) {
                    $args->{$k} = $v;
                } elsif (ref($args->{$k}) eq 'HASH') {
                    my($kk,$vv) = split(/:/,$v,2);
                    $args->{$k}->{$kk} = $vv;
                } else {
                    push(@{$args->{$k}}, $v);
                }
            } else {
                $args->{$k} = [] unless defined $args->{$k};
                push(@{$args->{$k}}, $v);
            }
        } elsif ($arg =~ /^(-{1,2}.*)$/) {
            my $k = qchomp($1);
            $k =~ s/^-+//;
            if ($k ne '_') {
                ++$args->{$k};
            } else {
                usage(qq{Cannot have an option named underscore});
            }
        } else {
            $args->{'_'} = [] unless defined $args->{'_'};
            push(@{$args->{'_'}}, $arg);
        }
    }
    $args->{'verbose'} = $args->{'v'}
        if (defined($args->{'v'}) && !defined($args->{'verbose'}));
    $::VERBOSE = $args->{'verbose'};
    return $args;
}

sub arrange_file_handles {
    my($filename) = @_;
    my($in,$out);
    if (!defined($filename)) {
        $in = IO::Handle->new();
        $in->fdopen(fileno(STDIN),"r") or die("fdopen(STDIN): $!\n");
        $out = IO::Handle->new();
        $out->fdopen(fileno(STDOUT),"w") or die("fdopen(STDOUT): $!\n");
    } else {
        $in = IO::File->new();
        my $input = $filename . ".in";
        $in->open("< $input") or die("$input: $!\n");
        $out = IO::File->new();
        $out->open("> $filename") or die("$filename: $!\n");
    }
    return($in,$out);
}

sub suss {
    my($args,$filename,$substitutions) = @_;
    my($in,$out) = arrange_file_handles($filename);
    while (defined(my $line = <$in>)) {
        foreach my $sub (@$substitutions) {
            my $var = '@' . $sub->[0] . '@';
            my $val = $sub->[1];
            $line =~ s/$var/$val/g;
            $out->print($line);
        }
    }
    $in->close();
    $out->close();
}

##

MAIN: {
    my $args = parse_argv({'_' => [],'file' => []}, @ARGV);
    usage() if $args->{'help'};
    my $subs = [ map { [split(/=/,$_,2)] } @{$args->{'_'}} ];
    my @files = @{$args->{'file'}};
    unshift(@files,undef) unless @files;
    suss($args,$_,$subs) foreach @files;
    exit(0);
}

__END__

=pod

program - purpose

=head1 SYNOPSIS

  # clever comment
  $ example command

=head1 DESCRIPTION

This program does it all.

=head1 OPTIONS

We accept the following optionology:

=over 4

=item -verbose (or -v)

=item -verbosity=int (or -V=int)

The first form increments the verbosity level every time it is seen.
The second form sets the verbosity level to the integer specified.
Higher verbosity levels mean more output.

=item -quiet

Be quiet about everything but errors.

=item -help

Print a short usage message.  If you specify -verbose, you get this
man page.

=item -version

Print our version

=back

=head1 VERSION HISTORY

B<Alice>: Well I must say I've never heard it that way before...

B<Caterpillar>: I know, I have improved it. 

Z<>

  0.1.0   16 Feb 07     snl     Started

=cut

##
# Local variables:
# mode: perl
# tab-width: 4
# perl-indent-level: 4
# perl-continued-statement-offset: 4
# indent-tabs-mode: nil
# comment-column: 40
# End:
##
