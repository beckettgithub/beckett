# beckett
0% sUNC roblox executor for macos 
You will have to update addresses and pointers. Also you will have to add a function for lua_gc yourself. Do not use for reference this is horrible code.
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
