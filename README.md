# libudx

udx is reliable, multiplexed, and congestion-controlled streams over udp.

## Building

CMake is used for building static and dynamic libraries for use outside of Node.js.

```sh
cmake -S . -B build
cmake --build build
```

Do note that the submodules need to be installed before building:
```
git submodule update --init --recursive
```


## Debugging

When debugging native code, make sure to configure a debug build:

```sh
cmake -S . -B build -D CMAKE_BUILD_TYPE=Debug
```

### Memory errors

To diagnose and debug memory errors, such as leaks and use-after-free, the collection of sanitizers provided by LLVM are recommended. The sanitizers can be enabled by passing additional `CFLAGS` and/or `LDFLAGS` to the build:

```sh
CFLAGS=<...> LDFLAGS=<...> cmake -S . -B build
```

To read more about the various sanitizers and how to use them, see:

- <https://clang.llvm.org/docs/AddressSanitizer.html>
- <https://clang.llvm.org/docs/MemorySanitizer.html>
- <https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html>
- <https://clang.llvm.org/docs/LeakSanitizer.html>

> :warning: LeakSanitizer is still experimental and currently requires a newer version of LLVM on macOS. If using Homebrew, `brew install llvm` and `CC=/usr/local/opt/llvm/bin/clang` should be sufficient.

## License

MIT
