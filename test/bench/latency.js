const test = require('brittle')
const b4a = require('b4a')
const byteSize = require('tiny-byte-size')
const UDX = require('../..')
const proxy = require('../helpers/proxy')

test('throughput, 600 ms latency ± 100 ms jitter', async (t) => {
  const udx = new UDX()

  const aSocket = udx.createSocket()
  aSocket.bind()

  const bSocket = udx.createSocket()
  bSocket.bind()

  const a = udx.createStream(1)
  const b = udx.createStream(2)

  const socket = await proxy({ from: aSocket, to: bSocket }, async () => {
    await delay()
  })

  a.connect(aSocket, 2, socket.address().port)
  b.connect(bSocket, 1, socket.address().port)

  let received = 0

  const elapsed = await t.execution(new Promise((resolve, reject) => {
    a
      .on('error', reject)
      .end(b4a.alloc(32768))

    b
      .on('error', reject)
      .on('data', (data) => {
        received += data.byteLength

        if (received === 32768) resolve()
      })
      .on('end', async () => {
        a.destroy()
        b.destroy()

        await aSocket.close()
        await bSocket.close()

        socket.close()
      })
  }))

  t.comment(byteSize.perSecond(received, elapsed))
})

function delay () {
  return new Promise((resolve) => {
    setTimeout(resolve, 500 + /* jitter */ (Math.random() * 200 | 0))
  })
}
