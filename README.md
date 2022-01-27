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

## LICENSE

MIT
