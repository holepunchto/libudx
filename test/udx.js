const test = require('brittle')
const UDX = require('../')

test('network interfaces', async function (t) {
  const udx = new UDX()

  const interfaces = udx.networkInterfaces()

  t.ok(interfaces.length >= 1, 'has at least 1')

  for (let i = 0; i < interfaces.length; i++) {
    const n = interfaces[i]

    t.test(`interface ${i}`, async function (t) {
      t.ok(typeof n.name === 'string', `name: ${n.name}`)
      t.ok(typeof n.host === 'string', `host: ${n.host}`)
      t.ok(n.family === 4, `family: ${n.family}`)
      t.ok(typeof n.internal === 'boolean', `internal: ${n.internal}`)
    })
  }
})
