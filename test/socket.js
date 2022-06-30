const test = require('brittle')
const UDX = require('../')

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
    t.alike(message, Buffer.from('hello'))
    t.is(host, '127.0.0.1')
    t.is(family, 4)
    t.is(port, a.address().port)
    a.close()
  })

  a.bind(0)
  await a.send(Buffer.from('hello'), a.address().port)
})

test('simple message ipv6', async function (t) {
  t.plan(4)

  const u = new UDX()
  const a = u.createSocket()

  a.on('message', function (message, { host, family, port }) {
    t.alike(message, Buffer.from('hello'))
    t.is(host, '::1')
    t.is(family, 6)
    t.is(port, a.address().port)
    a.close()
  })

  a.bind(0, '::1')
  await a.send(Buffer.from('hello'), a.address().port, '::1')
})

test('empty message', async function (t) {
  t.plan(1)

  const u = new UDX()
  const a = u.createSocket()

  a.on('message', function (message) {
    t.alike(message, Buffer.alloc(0))
    a.close()
  })

  a.bind(0)
  await a.send(Buffer.alloc(0), a.address().port)
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
    const buf = Buffer.from('a message')
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

  const flushed = a.send(Buffer.from('hello'), a.address().port)

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

test('send after close', async function (t) {
  const u = new UDX()

  const a = u.createSocket()

  a.bind(0)
  a.close()

  t.is(await a.send(Buffer.from('hello'), a.address().port), false)
})
