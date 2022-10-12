#!/bin/bash

cd mysql-5.0.15

dos2unix ./configure
chmod +x ./configure

dos2unix ./innobase/configure
chmod +x ./innobase/configure
 
dos2unix ./bdb/dist/configure
chmod +x ./bdb/dist/configure

./configure --with-debug --with-ndb-debug --with-plugin-innobase

make clean

make -j4

make install

cd -
