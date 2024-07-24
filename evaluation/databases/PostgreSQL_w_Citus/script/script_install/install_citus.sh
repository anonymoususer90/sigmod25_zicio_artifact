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

# Set compiler to clang
export CXX=gcc
#export CLANG=clang-12

SOURCE_DIR=$BASE_DIR"postgres/"
TARGET_DIR=$BASE_DIR"pgsql/"
PGSQL_BIN=$TARGET_DIR"bin/"
CITUS_DIR=$BASE_DIR"citus/"

# gdb
if [ "$GDB" == "YES" ]
then
COMPILE_OPTION+=" -ggdb -O0 -g3 -fno-omit-frame-pointer -DZICIO_DEBUG"
else
COMPILE_OPTION+=" -O3"
fi

# print zicio stats 
#COMPILE_OPTION+=" -DZICIO"
#COMPILE_OPTION+=" -DZICIO_STAT"
echo $COMPILE_OPTION

# install citus
cd $CITUS_DIR

make PG_CONFIG=$TARGET_DIR/bin/pg_config clean

# update PATH
export PATH=$PGSQL_BIN:$PATH

# configure citus
./configure CFLAGS="$COMPILE_OPTION" LIBS="-lzicio"

# build citus
make PG_CONFIG=$TARGET_DIR/bin/pg_config -j$(nproc) --silent

# install citus
make PG_CONFIG=$TARGET_DIR/bin/pg_config install -j$(nproc) --silent
