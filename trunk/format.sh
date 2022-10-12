
#!/bin/bash

cd mysql-5.0.15

clang-format -style=file -i   ./sql/sql_select.h
clang-format -style=file -i   ./sql/sql_select.cc

cd -

