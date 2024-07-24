#!/bin/bash

# Change to this-file-exist-path.
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"/

# Default
BASE_DIR="$DIR""../../../PostgreSQL/"
DATA_DIR="$BASE_DIR""data/"

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

    *)
          # unknown option
    ;;
esac
done

TARGET_DIR=$BASE_DIR"pgsql/"
BIN_DIR=$TARGET_DIR"bin/"

# server start
rm -rf "$DATA_DIR"
"$BIN_DIR""initdb" -D "$DATA_DIR"
