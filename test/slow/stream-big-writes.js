const test = require('brittle')
const isCI = require('is-ci')
const { makeTwoStreams } = require('../helpers')

writeALot(1)

writeALot(1024)

writeALot(1024 * 1024)

writeALot(1024 * 1024 * 1024)

writeALot(5 * 1024 * 1024 * 1024)

function writeALot (send) {
  test('write as fast as possible (' + fmt(send) + ')', { skip: isCI, timeout: 5 * 60 * 1000 }, async function (t) {
    t.timeout(10 * 60 * 1000)
    t.plan(5)

    const [a, b] = makeTwoStreams(t)

    const then = Date.now()

    let chunks = 0
    let recvBytes = 0
    let sentBytes = 0

    const buf = Buffer.alloc(Math.min(send, 65536))

    a.setInteractive(false)

    a.on('data', function (data) {
      chunks++
      recvBytes += data.byteLength
    })

    a.on('end', function () {
      const delta = Date.now() - then
      const perSec = Math.floor(sentBytes / delta * 1000)

      t.is(recvBytes, sentBytes, 'sent and recv ' + fmt(send))
      t.pass('total time was ' + delta + ' ms')
      t.pass('send rate was ' + fmt(perSec) + '/s (' + chunks + ' chunk[s])')

      a.end()
    })

    write()
    b.on('drain', write)

    a.on('close', function () {
      t.pass('a closed')
    })

    b.on('close', function () {
      t.pass('b closed')
    })

    function write () {
      while (sentBytes < send) {
        sentBytes += buf.byteLength
        if (!b.write(buf)) break
      }

      if (sentBytes >= send) b.end()
    }
  })

  function fmt (bytes) {
    if (bytes >= 1024 * 1024 * 1024) return (bytes / 1024 / 1024 / 1024).toFixed(1).replace(/\.0$/, '') + ' GB'
    if (bytes >= 1024 * 1024) return (bytes / 1024 / 1024).toFixed(1).replace(/\.0$/, '') + ' MB'
    if (bytes >= 1024) return (bytes / 1024).toFixed(1).replace(/\.0$/, '') + ' KB'
    return bytes + ' B'
  }
}
