const test = require('brittle')
const b4a = require('b4a')
const UDX = require('../')
const UDXSocket = require('../lib/socket')

test('can bind and close', async function (t) {
  const u = new UDX()
  const s = u.createSocket()

  s.bind(0)
  await s.close()

  t.pass()
})

test('can bind to ipv6 and close', async function (t) {
  const u = new UDX()
  const s = u.createSocket()

  s.bind(0, '::1')
  await s.close()

  t.pass()
})

test('bind is effectively sync', async function (t) {
  const u = new UDX()

  const a = u.createSocket()
  const b = u.createSocket()

  a.bind(0)

  t.ok(a.address().port, 'has bound')
  t.exception(() => b.bind(a.address().port))

  await a.close()
  await b.close()
})

test('simple message', async function (t) {
  t.plan(4)

  const u = new UDX()
  const a = u.createSocket()

  a.on('message', function (message, { host, family, port }) {
    t.alike(message, b4a.from('hello'))
    t.is(host, '127.0.0.1')
    t.is(family, 4)
    t.is(port, a.address().port)
    a.close()
  })

  a.bind(0)
  await a.send(b4a.from('hello'), a.address().port)
})

test('simple message ipv6', async function (t) {
  t.plan(4)

  const u = new UDX()
  const a = u.createSocket()

  a.on('message', function (message, { host, family, port }) {
    t.alike(message, b4a.from('hello'))
    t.is(host, '::1')
    t.is(family, 6)
    t.is(port, a.address().port)
    a.close()
  })

  a.bind(0, '::1')
  await a.send(b4a.from('hello'), a.address().port, '::1')
})

test('empty message', async function (t) {
  t.plan(1)

  const u = new UDX()
  const a = u.createSocket()

  a.on('message', function (message) {
    t.alike(message, b4a.alloc(0))
    a.close()
  })

  a.bind(0)
  await a.send(b4a.alloc(0), a.address().port)
})

test('echo sockets (250 messages)', async function (t) {
  t.plan(3)

  const u = new UDX()

  const a = u.createSocket()
  const b = u.createSocket()

  const send = []
  const recv = []
  let echoed = 0
  let flushed = 0

  a.on('message', function (buf, { host, port }) {
    echoed++
    a.send(buf, port, host)
  })

  b.on('message', function (buf) {
    recv.push(buf)

    if (recv.length === 250) {
      t.alike(send, recv)
      t.is(echoed, 250)
      t.is(flushed, 250)
      a.close()
      b.close()
    }
  })

  a.bind()

  while (send.length < 250) {
    const buf = b4a.from('a message')
    send.push(buf)
    b.send(buf, a.address().port).then(function () {
      flushed++
    })
  }
})

test('close socket while sending', async function (t) {
  const u = new UDX()
  const a = u.createSocket()

  a.bind()

  const flushed = a.send(b4a.from('hello'), a.address().port)

  a.close()

  t.is(await flushed, false)
})

test('close waits for all streams to close', async function (t) {
  t.plan(2)

  const u = new UDX()

  const a = u.createSocket()
  const s = u.createStream(1)

  s.connect(a, 2, 0)

  let aClosed = false
  let sClosed = false

  s.on('close', function () {
    t.not(aClosed, 'socket waits for streams')
    sClosed = true
  })

  a.on('close', function () {
    t.ok(sClosed, 'stream was closed before socket')
    aClosed = true
  })

  a.close()

  setTimeout(function () {
    s.destroy()
  }, 100)
})

test('open + close a bunch of sockets', async function (t) {
  const u = new UDX()

  const l = t.test('linear')
  let count = 0

  l.plan(5)
  loop()

  async function loop () {
    count++

    const a = u.createSocket()

    a.bind(0)
    l.pass('opened socket')
    await a.close()

    if (count < 5) loop()
  }

  await l

  const p = t.test('parallel')
  p.plan(5)

  for (let i = 0; i < 5; i++) {
    const a = u.createSocket()
    a.bind(0)
    a.close().then(function () {
      p.pass('opened and closed socket')
    })
  }

  await p
})

test('can bind to ipv6 and receive from ipv4', async function (t) {
  t.plan(4)

  const u = new UDX()

  const a = u.createSocket()
  const b = u.createSocket()

  a.on('message', async function (message, { host, family, port }) {
    t.alike(message, b4a.from('hello'))
    t.is(host, '127.0.0.1')
    t.is(family, 4)
    t.is(port, b.address().port)
    a.close()
    b.close()
  })

  a.bind(0, '::')
  b.bind(0)

  b.send(b4a.from('hello'), a.address().port, '127.0.0.1')
})

test('can bind to ipv6 and send to ipv4', async function (t) {
  t.plan(4)

  const u = new UDX()

  const a = u.createSocket()
  const b = u.createSocket()

  b.on('message', async function (message, { host, family, port }) {
    t.alike(message, b4a.from('hello'))
    t.is(host, '127.0.0.1')
    t.is(family, 4)
    t.is(port, a.address().port)
    a.close()
    b.close()
  })

  a.bind(0, '::')
  b.bind(0)

  a.send(b4a.from('hello'), b.address().port, '127.0.0.1')
})

test('send after close', async function (t) {
  const u = new UDX()

  const a = u.createSocket()

  a.bind(0)
  a.close()

  t.is(await a.send(b4a.from('hello'), a.address().port), false)
})

test('try send simple message', async function (t) {
  t.plan(4)

  const u = new UDX()
  const a = u.createSocket()

  a.on('message', function (message, { host, family, port }) {
    t.alike(message, b4a.from('hello'))
    t.is(host, '127.0.0.1')
    t.is(family, 4)
    t.is(port, a.address().port)
    a.close()
  })

  a.bind(0)
  a.trySend(b4a.from('hello'), a.address().port)
})

test('try send simple message ipv6', async function (t) {
  t.plan(4)

  const u = new UDX()
  const a = u.createSocket()

  a.on('message', function (message, { host, family, port }) {
    t.alike(message, b4a.from('hello'))
    t.is(host, '::1')
    t.is(family, 6)
    t.is(port, a.address().port)
    a.close()
  })

  a.bind(0, '::1')
  a.trySend(b4a.from('hello'), a.address().port, '::1')
})

test('try send empty message', async function (t) {
  t.plan(1)

  const u = new UDX()
  const a = u.createSocket()

  a.on('message', function (message) {
    t.alike(message, b4a.alloc(0))
    a.close()
  })

  a.bind(0)
  a.trySend(b4a.alloc(0), a.address().port)
})

test('close socket while try sending', async function (t) {
  t.plan(2)

  const u = new UDX()
  const a = u.createSocket()

  a.bind()

  a.on('message', function (message) {
    t.fail('should not receive message')
  })

  a.on('close', function () {
    t.pass()
  })

  t.is(a.trySend(b4a.from('hello'), a.address().port), undefined)

  a.close()
})

test('try send after close', async function (t) {
  t.plan(2)

  const u = new UDX()

  const a = u.createSocket()

  a.on('message', function (message) {
    t.fail('should not receive message')
  })

  a.on('close', function () {
    t.pass()
  })

  a.bind(0)
  a.close()

  t.is(a.trySend(b4a.from('hello'), a.address().port), undefined)
})

test('connect to invalid host ip', function (t) {
  t.plan(1)

  const u = new UDX()

  const a = u.createSocket()
  const s = u.createStream(1)
  t.teardown(() => s.destroy())

  const invalidHost = '0.-1.0.0'

  try {
    s.connect(a, 2, 0, invalidHost)
  } catch (error) {
    t.is(error.message, `${invalidHost} is not a valid IP address`)
  }
})

test('bind to invalid host ip', function (t) {
  t.plan(1)

  const u = new UDX()
  const a = u.createSocket()

  const invalidHost = '0.-1.0.0'

  try {
    a.bind(0, invalidHost)
  } catch (error) {
    t.is(error.message, `${invalidHost} is not a valid IP address`)
  }
})

test('send to invalid host ip', async function (t) {
  t.plan(1)

  const u = new UDX()
  const a = u.createSocket()

  a.bind(0)

  const invalidHost = '0.-1.0.0'

  try {
    await a.send(b4a.from('hello'), a.address().port, invalidHost)
  } catch (error) {
    t.is(error.message, `${invalidHost} is not a valid IP address`)
  }

  await a.close()
})

test('try send to invalid host ip', async function (t) {
  t.plan(1)

  const u = new UDX()
  const a = u.createSocket()

  a.bind(0)

  const invalidHost = '0.-1.0.0'

  try {
    a.trySend(b4a.from('hello'), a.address().port, invalidHost)
  } catch (error) {
    t.is(error.message, `${invalidHost} is not a valid IP address`)
  }

  await a.close()
})

test('send without bind', async function (t) {
  t.plan(1)

  const u = new UDX()

  const a = u.createSocket()
  const b = u.createSocket()

  b.on('message', function (message) {
    t.alike(message, b4a.from('hello'))
    a.close()
    b.close()
  })

  b.bind(0)
  await a.send(b4a.from('hello'), b.address().port)
})

test('try send without bind', async function (t) {
  t.plan(1)

  const u = new UDX()

  const a = u.createSocket()
  const b = u.createSocket()

  b.on('message', function (message) {
    t.alike(message, b4a.from('hello'))
    a.close()
    b.close()
  })

  b.bind(0)
  a.trySend(b4a.from('hello'), b.address().port)
})

test('get address without bind', async function (t) {
  const u = new UDX()
  const a = u.createSocket()
  t.is(a.address(), null)
  await a.close()
})

test('bind twice', async function (t) {
  t.plan(1)

  const u = new UDX()
  const a = u.createSocket()

  a.bind(0)

  try {
    a.bind(0)
  } catch (error) {
    t.is(error.message, 'Already bound')
  }

  await a.close()
})

test('bind while closing', function (t) {
  t.plan(1)

  const u = new UDX()
  const a = u.createSocket()

  a.close()

  try {
    a.bind(0)
  } catch (error) {
    t.is(error.message, 'Socket is closed')
  }
})

test('close twice', async function (t) {
  t.plan(1)

  const u = new UDX()
  const a = u.createSocket()

  a.bind(0)

  a.on('close', function () {
    t.pass()
  })

  a.close()
  a.close()
})

test('set TTL', async function (t) {
  t.plan(2)

  const u = new UDX()
  const a = u.createSocket()

  a.on('message', function (message) {
    t.alike(message, b4a.from('hello'))
    a.close()
  })

  try {
    a.setTTL(5)
  } catch (error) {
    t.is(error.message, 'Socket not active')
  }

  a.bind(0)
  a.setTTL(5)

  await a.send(b4a.from('hello'), a.address().port)
})

test('get recv buffer size', async function (t) {
  t.plan(2)

  const u = new UDX()
  const a = u.createSocket()

  try {
    a.getRecvBufferSize()
  } catch (error) {
    t.is(error.message, 'Socket not active')
  }

  a.bind(0)
  t.ok(a.getRecvBufferSize() > 0)

  await a.close()
})

test('set recv buffer size', async function (t) {
  t.plan(2)

  const u = new UDX()
  const a = u.createSocket()

  const NEW_BUFFER_SIZE = 8192

  try {
    a.setRecvBufferSize(NEW_BUFFER_SIZE)
  } catch (error) {
    t.is(error.message, 'Socket not active')
  }

  a.bind(0)
  a.setRecvBufferSize(NEW_BUFFER_SIZE)

  t.ok(a.getRecvBufferSize() >= NEW_BUFFER_SIZE)

  await a.close()
})

test('get send buffer size', async function (t) {
  t.plan(2)

  const u = new UDX()
  const a = u.createSocket()

  try {
    a.getSendBufferSize()
  } catch (error) {
    t.is(error.message, 'Socket not active')
  }

  a.bind(0)
  t.ok(a.getSendBufferSize() > 0)

  await a.close()
})

test('set send buffer size', async function (t) {
  t.plan(2)

  const u = new UDX()
  const a = u.createSocket()

  const NEW_BUFFER_SIZE = 8192

  try {
    a.setSendBufferSize(NEW_BUFFER_SIZE)
  } catch (error) {
    t.is(error.message, 'Socket not active')
  }

  a.bind(0)
  a.setSendBufferSize(NEW_BUFFER_SIZE)

  t.ok(a.getSendBufferSize() >= NEW_BUFFER_SIZE)

  await a.close()
})

test('UDXSocket - isIPv4', function (t) {
  t.is(UDXSocket.isIPv4('127.0.0.1'), true)
  t.is(UDXSocket.isIPv4('::1'), false)
  t.is(UDXSocket.isIPv4('0.-1.0.0'), false)
})

test('UDXSocket - isIPv6', function (t) {
  t.is(UDXSocket.isIPv6('127.0.0.1'), false)
  t.is(UDXSocket.isIPv6('::1'), true)
  t.is(UDXSocket.isIPv6('0.-1.0.0'), false)
})

test('UDXSocket - isIP', function (t) {
  t.is(UDXSocket.isIP('127.0.0.1'), 4)
  t.is(UDXSocket.isIP('::1'), 6)
  t.is(UDXSocket.isIP('0.-1.0.0'), 0)
})
