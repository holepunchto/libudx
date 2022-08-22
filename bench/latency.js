const bench = require('nanobench')
const b4a = require('b4a')
const byteSize = require('tiny-byte-size')
const UDX = require('..')
const proxy = require('../test/helpers/proxy')

bench('throughput, 600 ms latency ± 100 ms jitter', async ({ start, end, elapsed, log, error }) => {
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

  start()

  a
    .on('error', error)
    .end(b4a.alloc(32768))

  b
    .on('error', error)
    .on('data', (data) => {
      received += data.byteLength

      if (received === 32768) {
        log(byteSize(received / elapsed() * 1e3) + '/s')
      }
    })
    .on('end', async () => {
      a.destroy()
      b.destroy()

      await aSocket.close()
      await bSocket.close()

      socket.close()

      end()
    })
})

function delay () {
  return new Promise((resolve) => {
    setTimeout(resolve, 500 + /* jitter */ (Math.random() * 200 | 0))
  })
}
