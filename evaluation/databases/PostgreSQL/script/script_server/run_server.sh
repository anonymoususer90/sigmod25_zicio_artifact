#!/bin/bash

# Change to this-file-exist-path.
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"/

# Default
BASE_DIR="$DIR""../../../PostgreSQL/"
DATA_DIR=$BASE_DIR"data/"
LOGFILE=$BASE_DIR"logfile"

# Parse parameters.
for i in "$@"
do
case $i in
    -b=*|--base-dir=*)
    BASE_DIR="${i#*=}"
    shift
    ;;

    -d=*|--data-dir=*)
    DATA_DIR="${i#*=}"
    shift
    ;;

    -l=*|--logfile=*)
    LOGFILE="${i#*=}"
    shift
    ;;

    *)
          # unknown option
    ;;
esac
done

TARGET_DIR=$BASE_DIR"pgsql/"
BIN_DIR=$TARGET_DIR"bin/"

export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:$TARGET_DIR"lib"

rm -rf $LOGFILE

# server start
$BIN_DIR"pg_ctl" -D $DATA_DIR -l $LOGFILE start
