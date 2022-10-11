#!/bin/bash

./configure --with-debug --with-ndb-debug --with-plugin-innobase

make clean

make -j4

make install


