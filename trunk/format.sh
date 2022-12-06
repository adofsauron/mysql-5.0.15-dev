
#!/bin/bash

cd mysql-5.0.15

clang-format -style=file -i   ./sql/*.h
clang-format -style=file -i   ./sql/*.cc


clang-format -style=file -i ./innobase/btr/*.h                  
clang-format -style=file -i ./innobase/buf/*.h  
clang-format -style=file -i ./innobase/data/*.h  
clang-format -style=file -i ./innobase/dict/*.h  
clang-format -style=file -i ./innobase/dyn/*.h  
clang-format -style=file -i ./innobase/eval/*.h  
clang-format -style=file -i ./innobase/fil/*.h  
clang-format -style=file -i ./innobase/fsp/*.h  
clang-format -style=file -i ./innobase/fut/*.h  
clang-format -style=file -i ./innobase/ha/*.h  
clang-format -style=file -i ./innobase/ibuf/*.h  
clang-format -style=file -i ./innobase/include/*.h  
clang-format -style=file -i ./innobase/lock/*.h  
clang-format -style=file -i ./innobase/log/*.h  
clang-format -style=file -i ./innobase/mach/*.h  
clang-format -style=file -i ./innobase/mem/*.h  
clang-format -style=file -i ./innobase/mtr/*.h  
clang-format -style=file -i ./innobase/os/*.h  
clang-format -style=file -i ./innobase/page/*.h  
clang-format -style=file -i ./innobase/pars/*.h  
clang-format -style=file -i ./innobase/que/*.h  
clang-format -style=file -i ./innobase/read/*.h  
clang-format -style=file -i ./innobase/rem/*.h  
clang-format -style=file -i ./innobase/row/*.h  
clang-format -style=file -i ./innobase/srv/*.h  
clang-format -style=file -i ./innobase/sync/*.h  
clang-format -style=file -i ./innobase/thr/*.h  
clang-format -style=file -i ./innobase/trx/*.h  
clang-format -style=file -i ./innobase/usr/*.h  
clang-format -style=file -i ./innobase/ut/*.h      


clang-format -style=file -i ./innobase/btr/*.c                  
clang-format -style=file -i ./innobase/buf/*.c  
clang-format -style=file -i ./innobase/data/*.c  
clang-format -style=file -i ./innobase/dict/*.c  
clang-format -style=file -i ./innobase/dyn/*.c  
clang-format -style=file -i ./innobase/eval/*.c  
clang-format -style=file -i ./innobase/fil/*.c  
clang-format -style=file -i ./innobase/fsp/*.c  
clang-format -style=file -i ./innobase/fut/*.c  
clang-format -style=file -i ./innobase/ha/*.c  
clang-format -style=file -i ./innobase/ibuf/*.c  
clang-format -style=file -i ./innobase/include/*.c  
clang-format -style=file -i ./innobase/lock/*.c  
clang-format -style=file -i ./innobase/log/*.c  
clang-format -style=file -i ./innobase/mach/*.c  
clang-format -style=file -i ./innobase/mem/*.c  
clang-format -style=file -i ./innobase/mtr/*.c  
clang-format -style=file -i ./innobase/os/*.c  
clang-format -style=file -i ./innobase/page/*.c  
clang-format -style=file -i ./innobase/pars/*.c  
clang-format -style=file -i ./innobase/que/*.c  
clang-format -style=file -i ./innobase/read/*.c  
clang-format -style=file -i ./innobase/rem/*.c  
clang-format -style=file -i ./innobase/row/*.c  
clang-format -style=file -i ./innobase/srv/*.c  
clang-format -style=file -i ./innobase/sync/*.c  
clang-format -style=file -i ./innobase/thr/*.c  
clang-format -style=file -i ./innobase/trx/*.c  
clang-format -style=file -i ./innobase/usr/*.c  
clang-format -style=file -i ./innobase/ut/*.c      


cd -
