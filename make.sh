#!/bin/sh
##
# invoke make -f <variant_makefile> $*
#
# determine variant (GNU or BSD) based on the behavior of the make
# command.
##
variant=BSD
(make -v 2>&1 | grep -s GNU) && variant=GNU
mfile=${variant}makefile
[ ! -f $mfile ] && {
    echo $0: no makefile $mfile - using default
    mfile=Makefile
}
exec make -f ${mfile} $*
