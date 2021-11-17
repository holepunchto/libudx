const utp = require('utp-native')

const buf = Buffer.alloc(65536)
let sent = 0

if (process.argv[2]) {
  const [host, port] = process.argv.slice(2)
  const socket = utp.connect(Number(port), host)

  socket._utp.setSendBufferSize(2 * 1024 * 1024)
  socket._utp.setRecvBufferSize(2 * 1024 * 1024)
  socket.on('connect', function () {
    console.error('Connected')
    console.time()
    socket.on('drain', drain)
    drain()

    function drain () {
      if (sent >= 100 * 1024 * 1024) {
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
