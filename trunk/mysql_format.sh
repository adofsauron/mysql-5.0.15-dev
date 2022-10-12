
#!/bin/bash

cd mysql-5.0.15

clang-format -style=file -i   ./sql/*.h
clang-format -style=file -i   ./sql/*.cc

cd -
