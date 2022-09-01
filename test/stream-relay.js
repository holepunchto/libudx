const test = require('brittle')
const b4a = require('b4a')
const { makeTwoStreams } = require('./helpers')

test('relay', function (t) {
  t.plan(1)

  const [a, b] = makeTwoStreams(t)
  const [c, d] = makeTwoStreams(t)

  c.relayTo(b)
  b.relayTo(c)

  a.on('data', function (data) {
    t.alike(data, b4a.from('hello world'))

    a.destroy()
    b.destroy()
    c.destroy()
    d.destroy()
  })

  d.write('hello world')
})

test('relay, destroy immediately', function (t) {
  const [a, b] = makeTwoStreams(t)
  const [c, d] = makeTwoStreams(t)

  c.relayTo(b)
  b.relayTo(c)

  a.destroy()
  b.destroy()
  c.destroy()
  d.destroy()

  t.pass()
})
