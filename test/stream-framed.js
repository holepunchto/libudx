const test = require('brittle')
const b4a = require('b4a')
const { makeTwoStreams } = require('./helpers')

test('framed mode', function (t) {
  t.plan(1)

  const [a, b] = makeTwoStreams(t, { framed: true })

  b.on('data', (buffer) => {
    t.alike(buffer, b4a.from([0x2, 0x0, 0x0, 0x4, 0x5]))

    a.destroy()
    b.destroy()
  })

  a.write(b4a.from([0x2, 0x0, 0x0]))
  a.write(b4a.from([0x4, 0x5]))
})

test('framed mode, large message', function (t) {
  t.plan(1)

  const [a, b] = makeTwoStreams(t, { framed: true })

  const buf = b4a.alloc(3 + 1024 * 4096 /* 4 MiB */)

  buf[2] = 0x40

  const recv = []

  b
    .on('data', (buffer) => {
      recv.push(buffer)
    })
    .on('end', () => {
      t.alike(b4a.concat(recv), buf)

      a.destroy()
      b.destroy()
    })

  a.end(buf)
})

test('framed mode, several frames', function (t) {
  t.plan(1)

  const [a, b] = makeTwoStreams(t, { framed: true })

  const buf = b4a.from([0x2, 0x0, 0x0, 0x4, 0x5])

  const recv = []

  b
    .on('data', (buffer) => {
      recv.push(buffer)
    })
    .on('end', () => {
      t.alike(b4a.concat(recv), b4a.concat([buf, buf, buf]))

      a.destroy()
      b.destroy()
    })

  a.write(buf)
  a.write(buf)
  a.end(buf)
})

test('framed mode, invalid frame', function (t) {
  t.plan(1)

  const [a, b] = makeTwoStreams(t, { framed: true })

  const buf = b4a.from([0x1 /* too short, leftover data */, 0x0, 0x0, 0x4, 0x5])

  b
    .on('data', (buffer) => {
      t.alike(buffer, buf)

      a.destroy()
      b.destroy()
    })

  a.write(buf)
})
