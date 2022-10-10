#!/bin/bash

./configure --with-debug --with-plugin-innobase

make clean

make -j4

make install


