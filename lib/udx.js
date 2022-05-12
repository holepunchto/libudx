const binding = require('./binding')
const Socket = require('./socket')
const Stream = require('./stream')

module.exports = class UDX {
  constructor () {
    this._handle = Buffer.allocUnsafe(binding.sizeof_udx_t)
    binding.udx_napi_init(this._handle)
  }

  createSocket () {
    return new Socket(this)
  }

  createStream (id, opts) {
    return new Stream(this, id, opts)
  }
}
