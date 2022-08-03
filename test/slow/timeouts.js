const test = require('brittle')
const UDX = require('../../')

test('default firewall - same socket', async function (t) {
  t.plan(1)

  const u = new UDX()

  const socket = u.createSocket()
  socket.bind(0)

  const a = u.createStream(1)
  const b = u.createStream(2)

  b.on('data', function (data) {
    t.fail('default firewall should not allow to receive data')
  })

  a.on('error', function (error) {
    t.is(error.code, 'ETIMEDOUT')

    socket.close()
  })

  a.connect(socket, 2, socket.address().port)
  a.write(Buffer.from('hello'))
})
