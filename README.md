# libudx

udx is reliable, multiplexed, and congestion-controlled streams over udp.

## Delivery timeout

A stream closes with `UV_ETIMEDOUT`, and cancels its outstanding writes with
`UV_ECANCELED`, when sent data or a stream-end packet has been outstanding for a
time budget without the cumulative ACK advancing. The budget starts when the
first packet is sent from idle and restarts whenever the cumulative ACK
advances. Retransmissions, selective ACKs and duplicate ACKs do not restart it.

The default budget is the time the old limit of seven consecutive RTO
expirations took: `13 * RTO + min(2 * SRTT + 2, RTO)` with
`RTO = min(max(SRTT + 4 * RTTVAR, 1000), 30000)`, measured without backoff. That
is 13 seconds before an RTT is known and about 13 seconds on a LAN. The TLP term
is `0` before an RTT is known. Retransmission backoff does not extend the
budget: the last retransmission is scheduled one RTO before the deadline, and
the close happens at the deadline.

`udx_stream_set_delivery_timeout(stream, timeout_ms)` sets a different budget
for a stream, like `TCP_USER_TIMEOUT`. Values below three RTOs are raised to
three RTOs, and `0` restores the default.

If a retransmission timer fires more than one RTO late, because the event loop
was blocked or the process was suspended, the time the loop did not run is not
charged to the budget and at least one more retransmission is sent before the
stream can time out. This is done for at most six late firings per cumulative
ACK advance, so a loop that is late on every firing still times out.

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
