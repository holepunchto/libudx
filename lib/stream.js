const util = require('util')
const streamx = require('streamx')
const b4a = require('b4a')
const binding = require('./binding')
const ip = require('./ip')

const MAX_PACKET = 2048 // it's always way less than this, but whatevs
const BUFFER_SIZE = 65536 + MAX_PACKET

module.exports = class UDXStream extends streamx.Duplex {
  constructor (udx, id, opts = {}) {
    super({ mapWritable: toBuffer })

    this.udx = udx
    this.socket = null

    this._handle = b4a.allocUnsafe(binding.sizeof_udx_napi_stream_t)
    this._view = new Uint32Array(this._handle.buffer, this._handle.byteOffset, this._handle.byteLength >> 2)

    this._wreqs = []
    this._wfree = []

    this._sreqs = []
    this._sfree = []
    this._closed = false

    this._readBuffer = b4a.allocUnsafe(BUFFER_SIZE)

    this._onwrite = null
    this._ondestroy = null
    this._firewall = opts.firewall || firewallAll

    this.id = id
    this.remoteId = 0
    this.remoteHost = null
    this.remoteFamily = 0
    this.remotePort = 0

    this.userData = null

    binding.udx_napi_stream_init(this.udx._handle, this._handle, id, this,
      this._ondata,
      this._onend,
      this._ondrain,
      this._onack,
      this._onsend,
      this._onmessage,
      this._onclose,
      this._onfirewall,
      this._realloc
    )

    if (opts.seq) binding.udx_napi_stream_set_seq(this._handle, opts.seq)

    binding.udx_napi_stream_recv_start(this._handle, this._readBuffer)
  }

  get connected () {
    return this.socket !== null
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

  get localHost () {
    return this.socket ? this.socket.address().host : null
  }

  get localFamily () {
    return this.socket ? this.socket.address().family : 0
  }

  get localPort () {
    return this.socket ? this.socket.address().port : 0
  }

  setInteractive (bool) {
    if (!this._closed) return
    binding.udx_napi_stream_set_mode(this._handle, bool ? 0 : 1)
  }

  connect (socket, remoteId, port, host, opts = {}) {
    if (this._closed) return

    if (this.connected) throw new Error('Already connected')
    if (socket.closing) throw new Error('Socket is closed')

    if (typeof host === 'object') {
      opts = host
      host = null
    }

    if (!host) host = '127.0.0.1'

    const family = ip.isIP(host)
    if (!family) throw new Error(`${host} is not a valid IP address`)

    if (!socket.bound) socket.bind(0)

    this.remoteId = remoteId
    this.remotePort = port
    this.remoteHost = host
    this.remoteFamily = family
    this.socket = socket

    if (opts.ack) binding.udx_napi_stream_set_ack(this._handle, opts.ack)

    binding.udx_napi_stream_connect(this._handle, socket._handle, remoteId, port, host, family)

    this.socket._addStream(this)

    this.emit('connect')
  }

  async send (buffer) {
    if (!this.connected || this._closed) return false

    const id = this._allocSend()
    const req = this._sreqs[id]

    req.buffer = buffer

    const promise = new Promise((resolve) => {
      req.onflush = resolve
    })

    binding.udx_napi_stream_send(this._handle, req.handle, id, buffer)

    return promise
  }

  trySend (buffer) {
    if (!this.connected || this._closed) return

    const id = this._allocSend()
    const req = this._sreqs[id]

    req.buffer = buffer
    req.onflush = noop

    binding.udx_napi_stream_send(this._handle, req.handle, id, buffer)
  }

  _read (cb) {
    cb(null)
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

    if (drained) cb(null)
    else this._onwrite = cb
  }

  _final (cb) {
    const id = this._allocWrite()
    const req = this._wreqs[id]

    req.buffer = b4a.allocUnsafe(0)

    const drained = binding.udx_napi_stream_write_end(this._handle, req.handle, id, req.buffer) !== 0

    if (drained) cb(null)
    else this._onwrite = cb
  }

  _predestroy () {
    if (!this._closed) binding.udx_napi_stream_destroy(this._handle)
    this._closed = true
    this._writeContinue(null)
  }

  _destroy (cb) {
    if (this.connected) this._ondestroy = cb
    else cb(null)
  }

  _ondata (read) {
    const data = this._readBuffer.subarray(0, read)

    this.push(data)

    this._readBuffer = this._readBuffer.byteLength - read > MAX_PACKET
      ? this._readBuffer.subarray(read)
      : b4a.allocUnsafe(BUFFER_SIZE)

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

    onflush(err >= 0)

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
    this._closed = true

    if (this.socket) {
      this.socket._removeStream(this)
      this.socket = null
    }

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

  _realloc () {
    this._readBuffer = Buffer.allocUnsafe(BUFFER_SIZE)
    return this._readBuffer
  }

  _allocWrite () {
    if (this._wfree.length > 0) return this._wfree.pop()
    const handle = b4a.allocUnsafe(binding.sizeof_udx_stream_write_t)
    return this._wreqs.push({ handle, buffer: null }) - 1
  }

  _allocSend () {
    if (this._sfree.length > 0) return this._sfree.pop()
    const handle = b4a.allocUnsafe(binding.sizeof_udx_stream_send_t)
    return this._sreqs.push({ handle, buffer: null, resolve: null, reject: null }) - 1
  }
}

function noop () {}

function toBuffer (data) {
  return typeof data === 'string' ? b4a.from(data) : data
}

function firewallAll (socket, port, host) {
  return true
}
