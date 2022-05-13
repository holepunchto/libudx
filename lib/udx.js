const binding = require('./binding')
const Socket = require('./socket')
const Stream = require('./stream')
const b4a = require('b4a')

module.exports = class UDX {
  constructor () {
    this._handle = b4a.allocUnsafe(binding.sizeof_udx_t)
    binding.udx_napi_init(this._handle)
  }

  createSocket () {
    return new Socket(this)
  }

  createStream (id, opts) {
    return new Stream(this, id, opts)
  }
}
