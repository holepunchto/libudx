const test = require('brittle')
const UCP = require('../')

test('basic stream', async function (t) {
  t.plan(8)

  const [a, b] = makeTwoStreams()

  a
    .on('data', function (data) {
      t.alike(data, Buffer.from('echo: hello world'), 'a received echoed data')
    })
    .on('end', function () {
      t.pass('a ended')
    })
    .on('finish', function () {
      t.pass('a finished')
    })
    .on('close', function () {
      t.pass('a closed')
    })

  b
    .on('data', function (data) {
      t.alike(data, Buffer.from('hello world'), 'b received data')
      b.write(Buffer.concat([Buffer.from('echo: '), data]))
    })
    .on('end', function () {
      t.pass('b ended')
      b.end()
    })
    .on('finish', function () {
      t.pass('b finished')
    })
    .on('close', function () {
      t.pass('b closed')
    })

  a.write(Buffer.from('hello world'))
  a.end()
})

function makeTwoStreams () {
  const a = new UCP()
  const b = new UCP()

  a.bind()
  b.bind()

  const aStream = a.createStream()
  const bStream = b.createStream()

  aStream.connect(bStream.id, b.address().port, '127.0.0.1')
  bStream.connect(aStream.id, a.address().port, '127.0.0.1')

  return [aStream, bStream]
}
