#!/bin/sh
#
# usage: PKG_CONFIG_PATH=... installcheck-local.sh <path to syncevo header files> <path to syncevo libraries> <extra ld flags>
set -ex

DIR=`mktemp -d`
TMPFILE_CXX=$DIR/installcheck-local.cpp
TMPFILE_O=$DIR/installcheck-local.o
TMPFILE=$DIR/installcheck-local

rmtmp () {
    rm -f $TMPFILE $TMPFILE_CXX $TMPFILE_O
    rmdir $DIR
}
trap rmtmp EXIT

# check that c++ works, whatever it is
cat >$TMPFILE_CXX <<EOF
#include <iostream>
#include <memory>

int main(int argc, char **argv)
{
    std::cout << "hello world\n";
    std::shared_ptr<char> ptr;
    return 0;
}
EOF

# C++11 is needed for std::unique_ptr.
for CXX in "c++ -Wall -Werror -std=c++11" "g++ -Wall -Werror -std=c++11" "c++ -std=c++11" "g++ -std=c++11" ""; do
    if [ ! "$CXX" ]; then
        echo "no usable C++11 compiler, skipping tests"
        exit 0
    fi
    if $CXX $TMPFILE_CXX -o $TMPFILE; then
        break
    fi
done

for header in `cd $1 && echo  *`; do
    cat >$TMPFILE_CXX <<EOF
#include <syncevo/$header>

int main(int argc, char **argv)
{
    return 0;
}
EOF
    # header must be usable stand-alone, only glib is allowed
    $CXX "-I$2" $TMPFILE_CXX -c -o $TMPFILE_O `pkg-config --cflags glib-2.0` -DHAVE_GLIB
done

# link once to check that the libs are found;
# must take DESTDIR into account by adding -L<libdir> (skipped when equal to /usr/lib)
# and modifying any additional paths including that
pkg-config --libs syncevolution
env LD_LIBRARY_PATH=$3:$3/syncevolution:$LD_LIBRARY_PATH $CXX -v $TMPFILE_O -o $TMPFILE "-L$3" `pkg-config --libs syncevolution | sed -e "s;/usr/lib;$3;g"` $4
