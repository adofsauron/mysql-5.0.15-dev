#!/bin/bash

groupadd mysql
useradd -g mysql mysql

rm /usr/local/share/mysql/data -rf

mkdir -p /usr/local/share/mysql/data 

chown -R mysql:mysql /usr/local/share/mysql/data 

/usr/local/bin/mysql_install_db --user=mysql --datadir=/usr/local/share/mysql/data


chown -R mysql:mysql /usr/local/share/mysql/data 

