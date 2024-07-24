#!/bin/bash

# Change to this-file-exist-path.
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"/

# Default
BASE_DIR=$DIR"../../../PostgreSQL_w_Citus/"
LIB_DIR=$BASE_DIR"pgsql/lib/"
#LIB_HEADER_DIR="usr/include/"
LIB_HEADER_DIR=$BASE_DIR"../../../libzicio/include/"
CONFIGURE=YES
GDB=NO 
INSTALL_PREREQUISITE=NO
COMPILE_OPTION=

# Parse parameters.
for i in "$@"
do
case $i in
	-b=*|--base-dir=*)
	BASE_DIR="${i#*=}"
	shift
	;;

	-c=*|--compile-option=*)
	COMPILE_OPTION="${i#*=}"
	shift
	;;

	--no-configure)
	CONFIGURE=NO
	shift
	;;

	--install-prerequisites)
	INSTALL_PREREQUISITE=YES
	shift
	;;

	--gdb)
	GDB=YES
	shift
	;;

	*)
		# unknown option
	;;
esac
done

# Install prerequisites
if [ "$INSTALL_PREREQUISITE" == "YES" ]
then
	sudo apt-get install -y libreadline-dev llvm-14 clang-14
fi

SOURCE_DIR=$BASE_DIR"postgres/"
TARGET_DIR=$BASE_DIR"pgsql/"
CITUS_DIR=$BASE_DIR"citus/"

cd $SOURCE_DIR

./configure --silent

make clean -j --silent

# gdb
if [ "$GDB" == "YES" ]
then
COMPILE_OPTION+=" -ggdb -O0 -g3 -fno-omit-frame-pointer"
COMPILE_OPTION+=" -DSLEEP_ON_ASSERT -DABORT_AT_FAIL"
COMPILE_OPTION+=" -DZICIO_DEBUG"
else
COMPILE_OPTION+=" -O3"
fi

# print zicio stats 
#COMPILE_OPTION+=" -DZICIO"
echo $COMPILE_OPTION

# configure
if [ "$CONFIGURE" == "YES" ]
then
	echo "configure start"
	./configure --silent --prefix=$TARGET_DIR --enable-cassert \
		CFLAGS="$COMPILE_OPTION" --with-libs="$LIB_DIR" LIBS="-lzicio" \
		--with-segsize=10 --with-blocksize=8 \
		--with-llvm LLVM_CONFIG="llvm-config-14" CLANG="clang-14"
fi

echo "make start"
# make
make -j$(nproc) --silent

echo "make install start"
# make install
make install -j$(nproc) --silent
