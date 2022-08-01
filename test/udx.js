const test = require('brittle')
const UDX = require('../')

test('lookup', async function (t) {
  const udx = new UDX()

  const address = await udx.lookup('localhost', { family: 4 })

  t.is(address.host, '127.0.0.1')
  t.is(address.family, 4)
})

test('lookup ipv6', async function (t) {
  const udx = new UDX()

  const address = await udx.lookup('localhost', { family: 6 })

  t.is(address.host, '::1')
  t.is(address.family, 6)
})

test('lookup invalid', async function (t) {
  const udx = new UDX()

  t.exception(udx.lookup('example.invalid.'))
})

test('network interfaces', async function (t) {
  const udx = new UDX()

  const interfaces = udx.networkInterfaces()

  t.ok(interfaces.length >= 1, 'has at least 1')

  for (let i = 0; i < interfaces.length; i++) {
    const n = interfaces[i]

    t.test(`interface ${i}`, async function (t) {
      t.ok(typeof n.name === 'string', `name: ${n.name}`)
      t.ok(typeof n.host === 'string', `host: ${n.host}`)
      t.ok(n.family === 4 || n.family === 6, `family: ${n.family}`)
      t.ok(typeof n.internal === 'boolean', `internal: ${n.internal}`)
    })
  }
})

test('network interfaces - watch', async function (t) {
  t.plan(2)

  const udx = new UDX()
  // const initialInterfaces = udx.networkInterfaces().length

  const watcher = udx.watchNetworkInterfaces(function (interfaces) {
    // t.ok(initialInterfaces !== interfaces.length, 'total interfaces has changed')
    t.ok(interfaces.length >= 1, 'has at least 1')

    watcher.destroy()
  })

  watcher.once('close', function () {
    t.pass()
  })

  // sudo ip link add udx0test type dummy
  // sudo ip link set dev udx0test up
  // wait for watcher to catch up
  // sudo ip link set dev udx0test down
  // sudo ip link delete udx0test type dummy

  // Windows? Mac OS? Etcetera

  // As it's too complicated to, in every OS, adding and removing a temporal interface:
  watcher._onevent()
})

test('network interfaces - watch, unwatch and destroy twice', async function (t) {
  t.plan(3)

  const udx = new UDX()

  // This already does a watch()
  const watcher = udx.watchNetworkInterfaces(function (interfaces) {
    t.ok(interfaces.length >= 1, 'has at least 1')

    // Unwatch twice to trigger an internal condition that avoids unwatching twice
    watcher.unwatch()
    watcher.unwatch()

    // Destroy twice with the same intention
    const p1 = watcher.destroy()
    const p2 = watcher.destroy()
    t.is(p1, p2)
  })

  // This is the second watch() to trigger an internal condition that avoids watching twice
  watcher.watch()

  watcher.once('close', function () {
    t.pass()
  })

  watcher._onevent()
})
