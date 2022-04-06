const test = require('brittle')
const isCI = require('is-ci')
const Socket = require('../')

test('tiny echo stream', async function (t) {
  t.plan(8)

  const [a, b] = makeTwoStreams(t)

  a.on('data', function (data) {
    t.alike(data, Buffer.from('echo: hello world'), 'a received echoed data')
  })

  a.on('end', function () {
    t.pass('a ended')
  })

  a.on('finish', function () {
    t.pass('a finished')
  })

  a.on('close', function () {
    t.pass('a closed')
  })

  b.on('data', function (data) {
    t.alike(data, Buffer.from('hello world'), 'b received data')
    b.write(Buffer.concat([Buffer.from('echo: '), data]))
  })

  b.on('end', function () {
    t.pass('b ended')
    b.end()
  })

  b.on('finish', function () {
    t.pass('b finished')
  })

  b.on('close', function () {
    t.pass('b closed')
  })

  a.write(Buffer.from('hello world'))
  a.end()
})

test('end immediately', async function (t) {
  t.plan(6)

  const [a, b] = makeTwoStreams(t)

  a.on('data', function () {
    t.fail('should not send data')
  })

  a.on('end', function () {
    t.pass('a ended')
  })

  a.on('finish', function () {
    t.pass('a finished')
  })

  a.on('close', function () {
    t.pass('a closed')
  })

  b.on('data', function (data) {
    t.fail('should not send data')
  })

  b.on('end', function () {
    t.pass('b ended')
    b.end()
  })

  b.on('finish', function () {
    t.pass('b finished')
  })

  b.on('close', function () {
    t.pass('b closed')
  })

  a.end()
})

test('only one side writes', async function (t) {
  t.plan(7)

  const [a, b] = makeTwoStreams(t)

  a.on('data', function () {
    t.fail('should not send data')
  })

  a.on('end', function () {
    t.pass('a ended')
  })

  a.on('finish', function () {
    t.pass('a finished')
  })

  a.on('close', function () {
    t.pass('a closed')
  })

  b.on('data', function (data) {
    t.alike(data, Buffer.from('hello world'), 'b received data')
  })

  b.on('end', function () {
    t.pass('b ended')
    b.end()
  })

  b.on('finish', function () {
    t.pass('b finished')
  })

  b.on('close', function () {
    t.pass('b closed')
  })

  a.write(Buffer.from('hello world'))
  a.end()
})

test('unordered messages', async function (t) {
  t.plan(2)

  const [a, b] = makeTwoStreams(t)
  const expected = []

  b.on('message', function (buf) {
    b.send(Buffer.from('echo: ' + buf.toString()))
  })

  a.on('error', function () {
    t.pass('a destroyed')
  })

  a.on('message', function (buf) {
    expected.push(buf.toString())

    if (expected.length === 3) {
      t.alike(expected.sort(), [
        'echo: a',
        'echo: bc',
        'echo: d'
      ])

      // TODO: .end() here triggers a bug, investigate
      b.destroy()
    }
  })

  a.send(Buffer.from('a'))
  a.send(Buffer.from('bc'))
  a.send(Buffer.from('d'))
})

test('several streams on same socket', async function (t) {
  const socket = new Socket()
  socket.bind(0)

  t.teardown(() => socket.close())

  for (let i = 0; i < 10; i++) {
    const stream = Socket.createStream(i)
    stream.connect(socket, i, socket.address().port)

    t.teardown(() => stream.destroy())
  }

  t.pass('halts')
})

test('destroy unconnected stream', async function (t) {
  t.plan(1)

  const stream = Socket.createStream(1)

  stream.on('close', function () {
    t.pass('closed')
  })

  stream.destroy()
})

test('preconnect', async function (t) {
  t.plan(4)

  const socket = new Socket()
  socket.bind(0)

  socket.on('preconnect', (id, address) => {
    t.is(address.port, socket.address().port)
    t.is(address.address, '127.0.0.1')
    t.is(id, a.id)

    a.connect(socket, 2, socket.address().port)
    a.on('data', function (data) {
      t.is(data.toString(), 'hello')

      a.destroy()
      b.destroy()

      socket.close()
    })
  })

  const a = Socket.createStream(1)
  const b = Socket.createStream(2)

  b.connect(socket, 1, socket.address().port)
  b.write(Buffer.from('hello'))
})

test('destroy streams and close socket in callback', async function (t) {
  t.plan(1)

  const socket = new Socket()
  socket.bind(0)

  const a = Socket.createStream(1)
  const b = Socket.createStream(2)

  a.connect(socket, 2, socket.address().port)
  b.connect(socket, 1, socket.address().port)

  a.on('data', function (data) {
    a.destroy()
    b.destroy()

    socket.close(() => t.pass('closed'))
  })

  b.write(Buffer.from('hello'))
})

test('write empty buffer', async function (t) {
  t.plan(3)

  const [a, b] = makeTwoStreams(t)

  a
    .on('data', function (data) {
      t.alike(data, Buffer.alloc(0))
    })
    .on('close', function () {
      t.pass('a closed')
    })
    .end()

  b
    .on('close', function () {
      t.pass('b closed')
    })
    .end(Buffer.alloc(0))
})

writeALot(1)

writeALot(1024)

writeALot(1024 * 1024)

writeALot(1024 * 1024 * 1024)

writeALot(5 * 1024 * 1024 * 1024)

function writeALot (send) {
  test('write as fast as possible (' + fmt(send) + ')', { skip: isCI }, async function (t) {
    t.timeout(10 * 60 * 1000)
    t.plan(5)

    const [a, b] = makeTwoStreams(t)

    const then = Date.now()

    let chunks = 0
    let recvBytes = 0
    let sentBytes = 0

    const buf = Buffer.alloc(Math.min(send, 65536))

    a.setInteractive(false)

    a.on('data', function (data) {
      chunks++
      recvBytes += data.byteLength
    })

    a.on('end', function () {
      const delta = Date.now() - then
      const perSec = Math.floor(sentBytes / delta * 1000)

      t.is(recvBytes, sentBytes, 'sent and recv ' + fmt(send))
      t.pass('total time was ' + delta + ' ms')
      t.pass('send rate was ' + fmt(perSec) + '/s (' + chunks + ' chunk[s])')

      a.end()
    })

    write()
    b.on('drain', write)

    a.on('close', function () {
      t.pass('a closed')
    })

    b.on('close', function () {
      t.pass('b closed')
    })

    function write () {
      while (sentBytes < send) {
        sentBytes += buf.byteLength
        if (!b.write(buf)) break
      }

      if (sentBytes >= send) b.end()
    }
  })

  function fmt (bytes) {
    if (bytes >= 1024 * 1024 * 1024) return (bytes / 1024 / 1024 / 1024).toFixed(1).replace(/\.0$/, '') + ' GB'
    if (bytes >= 1024 * 1024) return (bytes / 1024 / 1024).toFixed(1).replace(/\.0$/, '') + ' MB'
    if (bytes >= 1024) return (bytes / 1024).toFixed(1).replace(/\.0$/, '') + ' KB'
    return bytes + ' B'
  }
}

function makeTwoStreams (t) {
  const a = new Socket()
  const b = new Socket()

  a.bind()
  b.bind()

  const aStream = Socket.createStream(1)
  const bStream = Socket.createStream(2)

  aStream.connect(a, bStream.id, b.address().port, '127.0.0.1')
  bStream.connect(b, aStream.id, a.address().port, '127.0.0.1')

  t.teardown(() => {
    a.close()
    b.close()
  })

  return [aStream, bStream]
}
