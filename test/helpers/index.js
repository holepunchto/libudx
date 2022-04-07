const Socket = require('../../')

module.exports = { makeTwoStreams }

function makeTwoStreams (t) {
  const a = new Socket()
  const b = new Socket()

  a.bind()
  b.bind()

  const aStream = Socket.createStream(1)
  const bStream = Socket.createStream(2)

  aStream.connect(a, bStream.id, b.address().port, '127.0.0.1')
  bStream.connect(b, aStream.id, a.address().port, '127.0.0.1')

  t.teardown(() => {
    a.close()
    b.close()
  })

  return [aStream, bStream]
}
