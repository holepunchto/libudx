# libudx

udx is reliable, multiplexed, and congestion-controlled streams over udp.

## Building

<https://github.com/holepunchto/bare-make> is used for building static and dynamic libraries for use outside of Node.js.

```sh
bare-make generate
bare-make build
```

## Debugging

When debugging native code, make sure to configure a debug build:

```sh
bare-make generate --debug
```

### Memory errors

To diagnose and debug memory errors, such as leaks and use-after-free, the collection of sanitizers provided by LLVM are recommended. The sanitizers can be enabled by passing the `--sanitize` flag when generating the build system:

```sh
bare-make generate --debug --sanitize <address|memory|undefined|leak|thread>
```

To read more about the various sanitizers and how to use them, see:

- <https://clang.llvm.org/docs/AddressSanitizer.html>
- <https://clang.llvm.org/docs/MemorySanitizer.html>
- <https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html>
- <https://clang.llvm.org/docs/LeakSanitizer.html>
- <https://clang.llvm.org/docs/ThreadSanitizer.html>

## License

Apache-2.0
