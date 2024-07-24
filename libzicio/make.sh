#!/bin/zsh

LNX=.

# find files which we want to anaylze
find . \
	  -name "*.[chxsS]" -print > $LNX"/cscope.files" 

echo "[SUCCESS] find all files"

# build cscope database
cscope -b -q -k -i $LNX"/cscope.files"

echo "[SUCCESS] cscope build is done"

ctags -R ./
# build ctag database
echo "[SUCCESS] ctag build is done"

