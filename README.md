# beckett
roblox executor for macos (not finished does not set capabilities. next update will be adding elevate proto, then all of the roblox globals after that then from there it will not be open source)
you will need to add luau and make your build script something like 
```
clang++ -dynamiclib \
    beckettMain.cpp \
    Luau/Compiler/src/*.cpp \
    Luau/Ast/src/*.cpp \
    Luau/VM/src/*.cpp \
    -I./Luau/Common/include \
    -I./Luau/Ast/include \
    -I./Luau/Compiler/include \
    -I./Luau/VM/include \
    -std=c++17 \
    -o libbeckett.dylib
```
then I inject using lib2proc
