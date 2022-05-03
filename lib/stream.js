const util = require('util')
const streamx = require('streamx')
const b4a = require('b4a')
const binding = require('./binding')

const MAX_PACKET = 2048 // it's always way less than this, but whatevs
const BUFFER_SIZE = 65536 + MAX_PACKET

module.exports = class UDXStream extends streamx.Duplex {
  constructor (udxHandle, id, opts = {}) {
    super({ mapWritable: toBuffer })

    this._socket = null
    this._udxHandle = udxHandle
    this._handle = Buffer.allocUnsafe(binding.sizeof_udx_napi_stream_t)
    this._view = new Uint32Array(this._handle.buffer, this._handle.byteOffset, this._handle.byteLength >> 2)

    this._wreqs = []
    this._wfree = []

    this._sreqs = []
    this._sfree = []

    this._readBuffer = Buffer.allocUnsafe(BUFFER_SIZE)

    this._onopen = null
    this._onwrite = null
    this._ondestroy = null
    this._firewall = opts.firewall || firewallAll

    this.connected = false

    this.id = id
    this.remoteId = 0

    binding.udx_napi_stream_init(this._udxHandle, this._handle, id, this,
      this._ondata,
      this._onend,
      this._ondrain,
      this._onack,
      this._onsend,
      this._onmessage,
      this._onclose,
      this._onfirewall
    )

    binding.udx_napi_stream_recv_start(this._handle, this._readBuffer)
  }

  get rtt () {
    return this._view[binding.offsetof_udx_stream_t_srtt >> 2]
  }

  get cwnd () {
    return this._view[binding.offsetof_udx_stream_t_cwnd >> 2]
  }

  get inflight () {
    return this._view[binding.offsetof_udx_stream_t_inflight >> 2]
  }

  setInteractive (bool) {
    if (!this._handle) return
    binding.udx_napi_stream_set_mode(this._handle, bool ? 0 : 1)
  }

  connect (socket, remoteId, port, host) {
    if (this._handle === null) return

    if (this.connected) throw new Error('Already connected')
    if (socket.closing) throw new Error('Socket is closed')

    if (!host) host = '127.0.0.1'

    if (!socket.bound) socket.bind(0)

    this.remoteId = remoteId
    this.connected = true

    this._socket = socket

    binding.udx_napi_stream_connect(this._handle, socket._handle, remoteId, port, host)

    socket.streams.add(this)
    this.on('close', () => {
      socket.streams.delete(this)
      socket._closeMaybe()
    })

    this._openContinue(null)
  }

  send (buffer, onflush) {
    if (!this.connected) throw new Error('Socket not connected')
    if (!onflush) onflush = noop

    if (this.destroying) return onflush(new Error('Socket destroyed'))

    const id = this._allocSend()
    const req = this._sreqs[id]

    req.buffer = buffer
    req.onflush = onflush

    binding.udx_napi_stream_send(this._handle, req.handle, id, buffer)
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
    const req = this._wreqs[id]

    req.buffer = buffer

    const drained = binding.udx_napi_stream_write(this._handle, req.handle, id, req.buffer) !== 0

    if (drained) return cb(null)
    else this._onwrite = cb
  }

  _final (cb) {
    const id = this._allocWrite()
    const req = this._wreqs[id]

    req.buffer = Buffer.allocUnsafe(0)

    const drained = binding.udx_napi_stream_write_end(this._handle, req.handle, id, req.buffer) !== 0

    if (drained) cb(null)
    else this._onwrite = cb
  }

  _predestroy () {
    if (this._handle && !binding.udx_napi_stream_destroy(this._handle)) {
      this._handle = null
    }
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

  _onend (read) {
    if (read > 0) this.push(this._readBuffer.subarray(0, read))
    this.push(null)
  }

  _ondrain () {
    this._writeContinue(null)
  }

  _onack (id) {
    const req = this._wreqs[id]

    req.buffer = null
    this._wfree.push(id)

    // gc the free list
    if (this._wfree.length >= 64 && this._wfree.length === this._wreqs.length) {
      this._wfree = []
      this._wreqs = []
    }
  }

  _onsend (id, err) {
    const req = this._sreqs[id]

    const onflush = req.onflush
    req.buffer = null
    req.onflush = null
    this._sfree.push(id)

    onflush(err < 0 ? new Error('Send failed') : null)

    // gc the free list
    if (this._sfree.length >= 16 && this._sfree.length === this._sreqs.length) {
      this._sfree = []
      this._sreqs = []
    }
  }

  _onmessage (buf) {
    this.emit('message', buf)
  }

  _onclose (errno) {
    // We are not allowed to touch the handle again from now on!
    this._handle = null

    // no error, we don't need to do anything
    if (errno === 0) return this._destroyContinue(null)

    let [code, msg] = util.getSystemErrorMap().get(errno)

    if (code === 'ECONNRESET') msg = 'stream destroyed by remote'
    else if (code === 'ETIMEDOUT') msg = 'stream timed out'

    msg = `${code}: ${msg}`

    const err = new Error(msg)
    err.errno = errno
    err.code = code

    if (this._ondestroy === null) this.destroy(err)
    else this._destroyContinue(err)
  }

  _onfirewall (socket, port, host) {
    return this._firewall(socket, port, host) ? 1 : 0
  }

  _allocWrite () {
    if (this._wfree.length > 0) return this._wfree.pop()
    const handle = Buffer.allocUnsafe(binding.sizeof_udx_stream_write_t)
    return this._wreqs.push({ handle, buffer: null }) - 1
  }

  _allocSend () {
    if (this._sfree.length > 0) return this._sfree.pop()
    const handle = Buffer.allocUnsafe(binding.sizeof_udx_stream_send_t)
    return this._sreqs.push({ handle, buffer: null, onflush: null }) - 1
  }
}

function noop () {}

function toBuffer (data) {
  return typeof data === 'string' ? b4a.from(data) : data
}

function firewallAll (socket, port, host) {
  return true
}
