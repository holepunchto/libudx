const test = require('brittle')
const byteSize = require('tiny-byte-size')
const { makePairs, pipeStreamPairs } = require('../helpers')

const STREAM_COUNTS = [1, 2, 4, 8]
const MESSAGE_SIZES = [1024 * 64, 1024]
const TRANSFER_SIZE = 1024 * 1024 * 64// send 64MB in total

for (const messageSize of MESSAGE_SIZES) {
  for (const streamCount of STREAM_COUNTS) {
    test(`throughput, ${streamCount} streams, 1 socket, message size ${messageSize}`, async t => {
      await benchmarkThroughput(t, streamCount, 'single', messageSize, TRANSFER_SIZE)
    })
    if (streamCount > 1) {
      test(`throughput, ${streamCount} streams, ${streamCount} sockets, message size ${messageSize}`, async t => {
        await benchmarkThroughput(t, streamCount, 'multi', messageSize, TRANSFER_SIZE)
      })
    }
  }
}

async function benchmarkThroughput (t, streamCount, multiplexMode, messageSize, total) {
  const { streams, close } = makePairs(streamCount, multiplexMode)
  const limit = total / streamCount

  const elapsed = await t.execution(async () => {
    try {
      await pipeStreamPairs(streams, messageSize, limit)
    } finally {
      close()
    }
  })

  t.comment(byteSize.perSecond(total, elapsed))
}
