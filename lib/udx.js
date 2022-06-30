const util = require('util')
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

  lookup (host, opts = {}) {
    const {
      family = 0
    } = opts

    const req = b4a.allocUnsafe(binding.sizeof_udx_napi_lookup_t)
    const ctx = {
      req,
      resolve: null,
      reject: null
    }

    const promise = new Promise((resolve, reject) => {
      ctx.resolve = resolve
      ctx.reject = reject
    })

    const errno = binding.udx_napi_lookup(req, host, family, ctx, onlookup)

    if (errno < 0) return Promise.reject(toError(errno))

    return promise
  }
}

function onlookup (errno, host, family) {
  if (errno < 0) this.reject(toError(errno))
  else this.resolve({ host, family })
}

function toError (errno) {
  const [code, msg] = util.getSystemErrorMap().get(errno)

  const err = new Error(`${code}: ${msg}`)
  err.errno = errno
  err.code = code

  Error.captureStackTrace(err, toError)

  return err
}
