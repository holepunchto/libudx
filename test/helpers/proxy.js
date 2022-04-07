const dgram = require('dgram')

// from udx.h

const UDX_HEADER_SIZE = 20
const UDX_MAGIC_BYTE = 0xff
const UDX_VERSION = 1
const UDX_HEADER_DATA = 0b00001
const UDX_HEADER_END = 0b00010
const UDX_HEADER_SACK = 0b00100
const UDX_HEADER_MESSAGE = 0b01000
const UDX_HEADER_DESTROY = 0b10000

module.exports = function proxy ({ from, to, bind } = {}, drop) {
  const socket = dgram.createSocket('udp4')

  socket.bind(bind || 0)

  socket.on('message', function (buf, rinfo) {
    const source = { host: rinfo.address, port: rinfo.port, peer: rinfo.port === from ? 'from' : (rinfo.port === to ? 'to' : 'unknown') }
    const pkt = parsePacket(buf)
    const dropping = drop(pkt, source)
    const port = rinfo.port === to ? from : to

    if (dropping && dropping.then) dropping.then(fwd)
    else fwd(dropping)

    function fwd (dropping) {
      if (dropping === true) return
      socket.send(buf, 0, buf.byteLength, port, '127.0.0.1')
    }
  })

  return new Promise((resolve) => {
    socket.on('listening', function () {
      resolve(socket)
    })
  })
}

function parsePacket (buf) {
  if (buf.byteLength < UDX_HEADER_SIZE || buf[0] !== UDX_MAGIC_BYTE || buf[1] !== UDX_VERSION) return { protocol: 'unknown', buffer: buf }

  const type = buf[2]
  const dataOffset = buf[3]

  return {
    protocol: 'udx',
    version: buf[1],
    isData: !!(type & UDX_HEADER_DATA),
    isEnd: !!(type & UDX_HEADER_END),
    isSack: !!(type & UDX_HEADER_SACK),
    isMessage: !!(type & UDX_HEADER_MESSAGE),
    isDestroy: !!(type & UDX_HEADER_DESTROY),
    stream: buf.readUint32LE(4),
    recv: buf.readUint32LE(8),
    seq: buf.readUint32LE(12),
    ack: buf.readUint32LE(16),
    additionalHeader: buf.subarray(UDX_HEADER_SIZE, UDX_HEADER_SIZE + dataOffset),
    data: buf.subarray(UDX_HEADER_SIZE + dataOffset)
  }
}
