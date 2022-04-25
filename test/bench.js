const Socket = require('..')
const bench = require('nanobench-utils/nanobench')

const STREAM_COUNTS = [1, 2, 4, 8]
const MESSAGE_SIZES = [1024 * 64, 1024]
const TRANSFER_SIZE = 1024 * 1024 * 64// send 64MB in total

let STREAM_ID = 1

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
  const { sockets, streams, close } = makePairs(streamCount, multiplexMode)
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
    b.end()
    close()
  }

  function onerror (err) {
    b.error(err)
    b.end()
    close()
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

function makePairs (n, multiplexMode) {
  const sockets = []
  const streams = []
  let a, b
  if (multiplexMode === 'single') {
    a = new Socket()
    b = new Socket()
    a.bind()
    b.bind()
    sockets.push(a, b)
  }
  while (streams.length < n) {
    let sa, sb
    if (multiplexMode === 'single') {
      sa = a
      sb = b
    } else {
      sa = new Socket()
      sb = new Socket()
      sa.bind()
      sb.bind()
      sockets.push(sa, sb)
    }
    const streamId = STREAM_ID++
    const aStream = Socket.createStream(streamId)
    const bStream = Socket.createStream(streamId)
    aStream.connect(sa, bStream.id, sb.address().port, '127.0.0.1')
    bStream.connect(sb, aStream.id, sa.address().port, '127.0.0.1')
    streams.push([aStream, bStream])
  }

  function close () {
    for (const pair of streams) {
      pair[0].end()
      pair[1].end()
    }
    for (const socket of sockets) {
      socket.close()
    }
  }

  return { sockets, streams, close }
}
