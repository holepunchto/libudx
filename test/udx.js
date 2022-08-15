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

  await t.exception(udx.lookup('example.invalid.'))
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

test('network interfaces - watch, unwatch and destroy twice', async function (t) {
  t.plan(2)

  const udx = new UDX()

  const watcher = udx.watchNetworkInterfaces()
  watcher.watch()

  const totalInterfaces = udx.networkInterfaces().length
  t.is(totalInterfaces, watcher.interfaces.length)

  watcher.unwatch()
  watcher.unwatch()

  watcher.destroy()
  watcher.destroy()

  watcher.once('close', function () {
    t.pass()
  })
})
