# libudx

udx is reliable, multiplex, and congestion controlled streams over udp.

It's written in C99 and depends on libuv.

It is pre-alpha wip (there be dragons), but at a stage where it's safe for developers to poke at.

The main purpose is to be a building block for P2P networking.
It therefore doesn't come with any handshaking protocol, encryption, and things like that - just reliable, fast, streams.

## API

TODO

## Node.js bindings

```
npm install udx-native
```

## Building

Two build setups are available: A GYP build and a CMake build.

### GYP

The GYP build is used for building a dynamic library for use in Node.js.

```sh
node-gyp configure
node-gyp build
```

The above commands are run as part of `npm install`.

### CMake

The CMake build is used for building static and dynamic libraries for use outside of Node.js.

```sh
cmake -S . -B build
cmake --build build
```

## Debugging

When debugging native code, make sure to configure a debug build:

```sh
# GYP
node-gyp configure --debug

# CMake
cmake -S . -B build -D CMAKE_BUILD_TYPE=Debug
```

### Memory errors

To diagnose and debug memory errors, such as leaks and use-after-free, the collection of sanitizers provided by LLVM are recommended. The sanitizers can be enabled by passing additional `CFLAGS` and/or `LDFLAGS` to the [CMake](#cmake) or [GYP](#gyp) build:

```sh
# GYP
CFLAGS=<...> LDFLAGS=<...> node-gyp build

# CMake
CFLAGS=<...> LDFLAGS=<...> cmake -S . -B build
```

To read more about the various sanitizers and how to use them, see:

- <https://clang.llvm.org/docs/AddressSanitizer.html>
- <https://clang.llvm.org/docs/MemorySanitizer.html>
- <https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html>
- <https://clang.llvm.org/docs/LeakSanitizer.html>

> :warning: LeakSanitizer is still experimental and currently requires a newer version of LLVM on macOS. If using Homebrew, `brew install llvm` and `CC=/usr/local/opt/llvm/bin/clang` should be sufficient.

## LICENSE

MIT
