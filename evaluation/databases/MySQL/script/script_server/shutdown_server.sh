#!/bin/bash

# Change to this-file-exist-path.
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"/

# Default
BASE_DIR="$DIR""../../../MySQL/"
BIN_DIR="$BASE_DIR"mysql/bin/
PASSWORD="zicio"

# Parse parameters.
for i in "$@"
do
case $i in
    -b=*|--base-dir=*)
    BASE_DIR="${i#*=}"
	BIN_DIR="$BASE_DIR"mysql/bin/
    shift
    ;;

    -d=*|--data-dir=*)
    DATA_DIR="${i#*=}"
    shift
	;;

    -i=*|--inst-dir=*)
    INST_DIR="${i#*=}"
	BIN_DIR="$INST_DIR"/bin/
    shift
	;;

	-p=*|--password=*)
	PASSWORD="${i#*=}"
	shift
    ;;

    *)
          # unknown option
    ;;
esac
done

$BIN_DIR"mysqladmin" -uroot -p"$PASSWORD" shutdown
