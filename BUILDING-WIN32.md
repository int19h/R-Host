Install [MSYS2](http://www.msys2.org/).

Open MSYS2 MSYS prompt (*not* MinGW 32-bit or 64-bit prompt!).

Install build tools from repo:
```sh
pacman -S base-devel make cmake mingw-w64-i686-toolchain mingw-w64-x86_64-toolchain mingw-w64-i686-cmake mingw-w64-x86_64-cmake 
pacman -S mingw-w64-x64_64-libzip mingw-w64-x86_64-boost
```

Install library dependencies from repo:
```sh
pacman -S mingw-w64-x64_64-libzip mingw-w64-x86_64-boost
```

Install minhook from a package file:
```sh
pacman -U ~/mingw-w64-x86_64-minhook-1.3.3-1-any.pkg.tar.xz

```

Close MSYS prompt, and open MinGW 64-bit prompt.

Go to src directory. Run CMake and make:
```sh
cmake .
make
```

The resulting binary will also be in src. It needs the following DLLs at runtime:

- libboost_filesystem-mt.dll
- libboost_program_options-mt.dll
- libboost_system-mt.dll
- libgcc_s_seh-1.dll
- libstdc++-6.dll
- libwinpthread-1.dll
- libzip-5.dll
- MinHook.dll

(These can be copied from C:\msys64\mingw64\bin)