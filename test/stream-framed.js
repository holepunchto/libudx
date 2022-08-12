const test = require('brittle')
const { makeTwoStreams } = require('./helpers')

test('framed mode', function (t) {
  t.plan(1)

  const [a, b] = makeTwoStreams(t, { framed: true })

  b.on('data', (buffer) => {
    t.alike(buffer, Buffer.of(0x2, 0x0, 0x0, 0x4, 0x5))

    a.destroy()
    b.destroy()
  })

  a.write(Buffer.of(0x2, 0x0, 0x0))
  a.write(Buffer.of(0x4, 0x5))
})
