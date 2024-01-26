#!/bin/sh

set -u

SRC='echo-test'
CompileOpts='-I../src -I../../TinyBase/src -Wall -mavx2 -fpermissive -lm -w'

mkdir -p ../build
cd ../build
gcc -o ${SRC} -g ../extra/${SRC}.c ${CompileOpts}
cd ../extra