# libudx

udx is reliable, multiplexed, and congestion-controlled streams over udp.

## Delivery timeout

Streams default to a 60-second limit without cumulative acknowledgment progress
while sent data or a stream-end packet remains unacknowledged. Expiry closes the
stream with `UV_ETIMEDOUT` and cancels outstanding writes with `UV_ECANCELED`.

Use `udx_stream_set_delivery_timeout(stream, timeout_ms)` to change the limit for a
stream. Zero disables this deadline; the existing limit of six RTO-driven
retransmissions per packet still applies. Changing the setting counts time already
elapsed since the first outstanding transmission or the last cumulative ACK
advance, including time when the deadline was disabled.

The deadline starts when the first outstanding reliable packet is sent, refreshes
when the cumulative ACK advances, and stops when all sent packets are cumulatively
acknowledged. Retransmissions, additional writes, duplicate ACKs, selective ACKs,
and unrelated incoming traffic do not refresh it. Idle streams and writes buffered
behind a zero receive window with no sent packets outstanding do not start this
deadline. Relay forwarding does not use this deadline; delivery is tracked by the
stream endpoints.

RTO backoff doubles the retransmission interval up to 30 seconds independently of
the delivery deadline. With a 1-second starting RTO and no ACKs, the nominal RTO
retransmissions occur at 1, 3, 7, 15, and 31 seconds before the 60-second deadline.
The starting RTO depends on the RTT estimate and retained backoff, so a larger RTO
allows fewer retries. This is a timeout-policy change: the previous behavior had
no fixed 13-second deadline and could tolerate longer loss periods at larger RTOs.

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
