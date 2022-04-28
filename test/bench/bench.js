const bench = require('nanobench-utils/nanobench')
const { makePairs, pipeStreamPairs } = require('../helpers')

const STREAM_COUNTS = [1, 2, 4, 8]
const MESSAGE_SIZES = [1024 * 64, 1024]
const TRANSFER_SIZE = 1024 * 1024 * 64// send 64MB in total

for (const messageSize of MESSAGE_SIZES) {
  for (const streamCount of STREAM_COUNTS) {
    bench(`throughput, ${streamCount} streams, 1 socket, message size ${messageSize}`, b => {
      benchmarkThroughput(b, streamCount, 'single', messageSize, TRANSFER_SIZE)
    })
    if (streamCount > 1) {
      bench(`throughput, ${streamCount} streams, ${streamCount} sockets, message size ${messageSize}`, b => {
        benchmarkThroughput(b, streamCount, 'multi', messageSize, TRANSFER_SIZE)
      })
    }
  }
}

function benchmarkThroughput (b, streamCount, multiplexMode, messageSize, total) {
  const { streams, close } = makePairs(streamCount, multiplexMode)
  const limit = total / streamCount

  b.throughput(total)
  b.start()
  pipeStreamPairs(streams, messageSize, limit)
    .then(() => {
      b.end()
      close()
    })
    .catch((err) => {
      b.fail(err)
      close()
    })
}
