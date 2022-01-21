const test = require('brittle')
const Socket = require('../')

test('can bind and close', async function (t) {
  t.plan(2)

  const s = new Socket()

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

  const a = new Socket()
  const b = new Socket()

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

  const a = new Socket()

  a.on('message', function (message, { address, port }) {
    t.alike(message, Buffer.from('hello'))
    t.is(address, '127.0.0.1')
    t.is(port, a.address().port)
    a.close()
  })

  a.bind(0)
  a.send(Buffer.from('hello'), 0, 5, a.address().port, '127.0.0.1')
})

test('echo sockets (1000 messages)', async function (t) {
  t.plan(3)

  const a = new Socket()
  const b = new Socket()

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

    if (recv.length === 1000) {
      t.alike(send, recv)
      t.is(echoed, 1000)
      t.is(flushed, 1000)
      a.close()
      b.close()
    }
  })

  a.bind()

  while (send.length < 1000) {
    const buf = Buffer.from('a message')
    send.push(buf)
    b.send(buf, 0, buf.byteLength, a.address().port, '127.0.0.1', function () {
      flushed++
    })
  }
})

test('close socket while sending', async function (t) {
  t.plan(2)

  const a = new Socket()

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

  const a = new Socket()
  const s = a.createStream()

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
