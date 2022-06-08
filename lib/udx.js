const b4a = require('b4a')
const binding = require('./binding')
const Socket = require('./socket')
const Stream = require('./stream')
const NetworkInterfaces = require('./network-interfaces')

module.exports = class UDX {
  constructor () {
    this._handle = b4a.allocUnsafe(binding.sizeof_udx_t)
    this._watchers = new Set()

    binding.udx_napi_init(this._handle)
  }

  createSocket () {
    return new Socket(this)
  }

  createStream (id, opts) {
    return new Stream(this, id, opts)
  }

  networkInterfaces () {
    let [watcher = null] = this._watchers
    if (watcher) return watcher.interfaces

    watcher = new NetworkInterfaces()
    watcher.destroy()

    return watcher.interfaces
  }

  watchNetworkInterfaces (onchange) {
    const watcher = new NetworkInterfaces()

    this._watchers.add(watcher)
    watcher.on('close', () => {
      this._watchers.delete(watcher)
    })

    if (onchange) watcher.on('change', onchange)

    return watcher.watch()
  }
}
