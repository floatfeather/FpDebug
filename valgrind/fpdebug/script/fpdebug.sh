#!/bin/bash

# This script assumes that you use and only use the VALGRIND_PRINT_VALUES once in your file.

valgrind --tool=fpdebug --goto-shadow-branch=yes $@ > shadow.fd.temp 2>&1
valgrind --tool=fpdebug $@ > original.fd.temp 2>&1

rm -rf fpdebug_relerr.log

# g++ fd_relerr.cpp -lmpfr -lgmp -o fd_relerr
/home/lillian/work/install_fpdebug/valgrind-3.7.0/fpdebug/script/fd_relerr

rm shadow.fd.temp
rm original.fd.temp