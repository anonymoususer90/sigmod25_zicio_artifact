#!/bin/bash

# Change to this-file-exist-path.
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"/
cd $DIR

# Default
BASE_DIR="$DIR""../../../MySQL/"
DBUSER=root
PASSWORD=$USER
PORT="23456"


# Parse parameters.
for i in "$@"
do
case $i in
    -b=*|--base-dir=*)
    BASE_DIR="${i#*=}"
    shift
    ;;

    -u=*|--user=*)
    DBUSER="${i#*=}"
    shift
    ;;

    -p=*|--port=*)
    PORT="${i#*=}"
    shift
    ;;

	--password=*)
	PASSWORD="${i#*=}"
	shift
	;;

    *)
          # unknown option
    ;;
esac
done


TARGET_DIR=$BASE_DIR"mysql/"
BIN_DIR=$TARGET_DIR"bin/"

LD_LIBRARY_PATH=$TARGET_DIR"lib/"
export LD_LIBRARY_PATH

# run client
"$BIN_DIR""mysql" -u${DBUSER} -p${PASSWORD}
