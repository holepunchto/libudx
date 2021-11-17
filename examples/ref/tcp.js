const net = require('net')

const buf = Buffer.alloc(65536)
let sent = 0

if (process.argv[2]) {
  const [host, port] = process.argv.slice(2)
  const socket = net.connect(port, host)

  socket.on('connect', function () {
    console.error('Connected')
    console.time()
    socket.on('drain', drain)
    drain()

    function drain () {
      if (sent >= 10 * 1024 * 1024) {
        console.timeEnd()
        console.log('Sent ' + sent + 'b')
        process.exit()
        return
      }
      do {
        sent += buf.byteLength
      } while (socket.write(buf) !== false)
    }
  })
} else {
  net.createServer(function (socket) {
    socket.resume()
    socket.on('error', () => socket.destroy())
  }).listen(0, function () {
    console.log('Listening on port ' + this.address().port)
  })
}
