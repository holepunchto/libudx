const utp = require('utp-native')
const speed = require('speedometer')()

const buf = Buffer.alloc(65536)
let sent = 0
let warm = false

if (process.argv[2]) {
  const [host, port] = process.argv.slice(2)
  const socket = utp.connect(Number(port), host)

  socket.on('connect', function () {
    socket._utp.setSendBufferSize(2 * 1024 * 1024)
    socket._utp.setRecvBufferSize(2 * 1024 * 1024)
    console.error('Connected')
    console.time()
    socket.on('drain', drain)
    drain()

    function drain () {
      if (sent >= 100 * 1024 * 1024 && !warm) {
        console.time('warm')
        warm = true
      }
      if (sent >= 1000 * 1024 * 1024) {
        console.timeEnd()
        console.timeEnd('warm')
        console.log('Done! Sent ' + sent + 'b')
        process.exit()
        return
      }
      do {
        speed(buf.byteLength)
        sent += buf.byteLength
      } while (socket.write(buf) !== false)
    }

    setInterval(function () {
      console.log((8 * speed() / 1000 / 1000).toFixed(2))
    }, 1000)
  })
} else {
  const server = utp.createServer(function (socket) {
    socket.resume()
    socket.on('error', () => socket.destroy())
  })
  server.setSendBufferSize(2 * 1024 * 1024)
  server.setRecvBufferSize(2 * 1024 * 1024)
  server.listen(0, function () {
    console.log('Listening on port ' + this.address().port)
  })
}
