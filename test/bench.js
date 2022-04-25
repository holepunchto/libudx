const Socket = require('..')
const bench = require('nanobench-utils/nanobench')

const STREAM_COUNTS = [1, 2, 4, 8]
const MESSAGE_SIZES = [1024 * 64, 1024]
const TRANSFER_SIZE = 1024 * 1024 * 64// send 64MB in total

let STREAM_ID = 1

for (const messageSize of MESSAGE_SIZES) {
  for (const streamCount of STREAM_COUNTS) {
    bench(`throughput with ${streamCount} streams and message size ${messageSize}`, b => {
      benchmarkThroughput(b, streamCount, messageSize, TRANSFER_SIZE)
    })
  }
}

function benchmarkThroughput (b, streamCount, messageSize, total) {
  const { sockets, streams } = makePairs(streamCount)
  const msg = Buffer.alloc(messageSize).fill('x')
  const limit = total / streamCount

  b.throughput(total)
  b.start()

  const readProms = []
  for (const pair of streams) {
    const [streamA, streamB] = pair
    write(streamA, limit, msg).catch(onerror)
    const readProm = read(streamB, limit).then(() => console.log(`stream ${streamA.id} finished`))
    readProms.push(readProm)
  }

  Promise.all(readProms).then(onend).catch(onerror)

  function onend () {
    onerror()
  }

  function onerror (err) {
    if (err) b.error(err)
    b.end()
    for (const pair of streams) {
      pair[0].destroy()
      pair[1].destroy()
    }
    sockets[0].close()
    sockets[1].close()
  }
}

function write (s, limit, msg) {
  return new Promise((resolve, reject) => {
    let written = 0
    s.once('error', reject)
    write()
    function write () {
      let floating = true
      while (floating && written < limit) {
        floating = s.write(msg)
        written += msg.length
      }
      if (written >= limit) {
        // console.log(s.id, 'wrote', written)
        resolve()
      } else {
        s.once('drain', write)
      }
    }
  })
}

function read (s, limit) {
  return new Promise((resolve, reject) => {
    let read = 0
    s.once('error', reject)
    s.on('data', (data) => {
      read += data.length
      if (read >= limit) {
        // console.log(s.id, 'read', read)
        resolve()
      }
    })
  })
}

function makePairs (n) {
  const a = new Socket()
  const b = new Socket()

  a.bind()
  b.bind()

  const sockets = [a, b]
  const streams = []
  while (streams.length < n) {
    const streamId = STREAM_ID++
    const aStream = Socket.createStream(streamId)
    const bStream = Socket.createStream(streamId)
    aStream.connect(a, bStream.id, b.address().port, '127.0.0.1')
    bStream.connect(b, aStream.id, a.address().port, '127.0.0.1')
    streams.push([aStream, bStream])
  }

  return { sockets, streams }
}
