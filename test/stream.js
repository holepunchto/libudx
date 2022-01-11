const test = require('brittle')
const UCP = require('../')

test('tiny echo stream', async function (t) {
  t.plan(8)

  const [a, b] = makeTwoStreams(t)

  a.on('data', function (data) {
    t.alike(data, Buffer.from('echo: hello world'), 'a received echoed data')
  })

  a.on('end', function () {
    t.pass('a ended')
  })

  a.on('finish', function () {
    t.pass('a finished')
  })

  a.on('close', function () {
    t.pass('a closed')
  })

  b.on('data', function (data) {
    t.alike(data, Buffer.from('hello world'), 'b received data')
    b.write(Buffer.concat([Buffer.from('echo: '), data]))
  })

  b.on('end', function () {
    t.pass('b ended')
    b.end()
  })

  b.on('finish', function () {
    t.pass('b finished')
  })

  b.on('close', function () {
    t.pass('b closed')
  })

  a.write(Buffer.from('hello world'))
  a.end()
})

function makeTwoStreams (t) {
  const a = new UCP()
  const b = new UCP()

  a.bind()
  b.bind()

  const aStream = a.createStream()
  const bStream = b.createStream()

  aStream.connect(bStream.id, b.address().port, '127.0.0.1')
  bStream.connect(aStream.id, a.address().port, '127.0.0.1')

  t.teardown(() => {
    a.close()
    b.close()
  })

  return [aStream, bStream]
}
