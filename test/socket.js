const test = require('brittle')
const UDX = require('../')

test('can bind and close', async function (t) {
  t.plan(2)

  const u = new UDX()
  const s = u.createSocket()

  s.bind(0, function () {
    t.pass('socket is listening')
    s.close()
  })

  s.on('close', function () {
    t.pass('socket closed')
  })
})

test('bind is effectively sync', async function (t) {
  t.plan(4)

  const u = new UDX()

  const a = u.createSocket()
  const b = u.createSocket()

  a.bind(0)

  t.ok(a.address().port, 'has bound')
  t.exception(() => b.bind(a.address().port))

  a.close()
  b.close()

  a.on('close', () => t.pass('a closed'))
  b.on('close', () => t.pass('b closed'))
})

test('simple message', async function (t) {
  t.plan(3)

  const u = new UDX()

  const a = u.createSocket()

  a.on('message', function (message, { address, port }) {
    t.alike(message, Buffer.from('hello'))
    t.is(address, '127.0.0.1')
    t.is(port, a.address().port)
    a.close()
  })

  a.bind(0)
  a.send(Buffer.from('hello'), 0, 5, a.address().port, '127.0.0.1')
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
  a.send(Buffer.alloc(0), 0, 0, a.address().port, '127.0.0.1')
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

  a.on('message', function (buf, { address, port }) {
    echoed++
    a.send(buf, 0, buf.byteLength, port, address)
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
    b.send(buf, 0, buf.byteLength, a.address().port, '127.0.0.1', function () {
      flushed++
    })
  }
})

test('close socket while sending', async function (t) {
  t.plan(2)

  const u = new UDX()

  const a = u.createSocket()

  a.bind()

  a.send(Buffer.from('hello'), 0, 5, a.address().port, '127.0.0.1', function (err) {
    t.ok(err, 'send was not flushed')
  })

  a.send(Buffer.from('world'), 0, 5, a.address().port, '127.0.0.1', function (err) {
    t.ok(err, 'send was not flushed')
  })

  a.close()
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

  function loop () {
    count++

    const a = u.createSocket()

    a.bind(0)
    l.pass('opened socket')
    a.close(function () {
      if (count === 5) return
      loop()
    })
  }

  await l

  const p = t.test('parallel')
  p.plan(5)

  for (let i = 0; i < 5; i++) {
    const a = u.createSocket()
    a.bind(0)
    a.close(function () {
      p.pass('opened and closed socket')
    })
  }

  await p
})
