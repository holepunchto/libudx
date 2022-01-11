const streamx = require('streamx')
const binding = require('./binding')

const MAX_PACKET = 2048 // it's always way less than this, but whatevs
const BUFFER_SIZE = 65536 + MAX_PACKET

module.exports = class UCPStream extends streamx.Duplex {
  constructor (socket) {
    super()

    this._socket = socket
    this._handle = Buffer.allocUnsafe(binding.sizeof_ucp_napi_stream_t)
    this._reqs = []
    this._free = []
    this._readBuffer = null

    this._onopen = null
    this._onwrite = null
    this._ondestroy = null

    this.connected = false
    this.remoteId = 0

    this.id = binding.ucp_napi_stream_init(socket._handle, this._handle, this,
      this._ondata,
      this._onend,
      this._ondrain,
      this._onack,
      this._onclose
    )
  }

  debug () {
    const handle = this._handle

    return {
      inflight: uint32(binding.offsetof_ucp_stream_t_inflight),
      cwnd: uint32(binding.offsetof_ucp_stream_t_cwnd),
      srtt: uint32(binding.offsetof_ucp_stream_t_srtt),
      pkts_waiting: uint32(binding.offsetof_ucp_stream_t_pkts_waiting),
      pkts_inflight: uint32(binding.offsetof_ucp_stream_t_pkts_inflight)
    }

    function uint32 (off) {
      const u = new Uint32Array(handle.buffer, handle.byteOffset + off, 1)
      return u[0]
    }
  }

  connect (remoteId, port, host) {
    if (this._handle === null) return

    if (this.connected) throw new Error('Already connected')
    if (!host) host = '127.0.0.1'
    this.remoteId = remoteId
    this.connected = true
    this._readBuffer = Buffer.allocUnsafe(BUFFER_SIZE)
    binding.ucp_napi_stream_connect(this._handle, this._readBuffer, remoteId, port, host)

    this._openContinue(null)
  }

  _read (cb) {
    cb(null)
  }

  _openContinue (err) {
    if (this._onopen === null) return
    const cb = this._onopen
    this._onopen = null
    cb(err)
  }

  _writeContinue (err) {
    if (this._onwrite === null) return
    const cb = this._onwrite
    this._onwrite = null
    cb(err)
  }

  _destroyContinue (err) {
    if (this._ondestroy === null) return
    const cb = this._ondestroy
    this._ondestroy = null
    cb(err)
  }

  _write (buffer, cb) {
    const id = this._allocWrite()
    const req = this._reqs[id]

    req.buffer = buffer

    const drained = binding.ucp_napi_stream_write(this._handle, req.handle, id, buffer) !== 0

    if (drained) return cb(null)
    else this._onwrite = cb
  }

  _final (cb) {
    const id = this._allocWrite()
    const req = this._reqs[id]

    req.buffer = null

    const drained = binding.ucp_napi_stream_end(this._handle, req.handle, id) !== 0

    if (drained) cb(null)
    else this._onwrite = cb
  }

  _predestroy () {
    if (this._handle) binding.ucp_napi_stream_destroy(this._handle)
    this._openContinue(null)
    this._writeContinue(null)
  }

  _destroy (cb) {
    if (this._handle === null) return cb(null)
    this._ondestroy = cb
  }

  _ondata (read) {
    const data = this._readBuffer.subarray(0, read)

    this.push(data)

    this._readBuffer = this._readBuffer.byteLength - read > MAX_PACKET
      ? this._readBuffer.subarray(read)
      : Buffer.allocUnsafe(BUFFER_SIZE)

    return this._readBuffer
  }

  _onend () {
    this.push(null)
  }

  _ondrain () {
    this._writeContinue(null)
  }

  _onack (id) {
    const req = this._reqs[id]

    req.buffer = null
    this._free.push(id)

    // gc the free list
    if (this._free.length >= 64 && this._free.length === this._reqs.length) {
      this._free = []
      this._reqs = []
    }
  }

  _onclose (err) {
    // We are not allowed to touch the handle again from now on!
    this._handle = null

    // no error, we don't need to do anything
    if (err === 0) return this._destroyContinue(null)

    // we destroyed it, that's fine, no need for anything
    if (err === -1) return this._destroyContinue(null)

    const msg = err === -2
      ? 'Stream destroyed by remote'
      : 'Stream timed out'

    const error = new Error(msg)

    if (this._ondestroy === null) this.destroy(error)
    else this._destroyContinue(error)
  }

  _allocWrite () {
    if (this._free.length > 0) return this._free.pop()
    const handle = Buffer.allocUnsafe(binding.sizeof_ucp_write_t)
    return this._reqs.push({ handle, buffer: null }) - 1
  }
}
