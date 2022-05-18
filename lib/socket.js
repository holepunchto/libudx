const binding = require('./binding')
const events = require('events')
const b4a = require('b4a')

const IPv4Pattern = /^((?:[0-9]|[1-9][0-9]|1[0-9][0-9]|2[0-4][0-9]|25[0-5])[.]){3}(?:[0-9]|[1-9][0-9]|1[0-9][0-9]|2[0-4][0-9]|25[0-5])$/

module.exports = class UDXSocket extends events.EventEmitter {
  constructor (udx) {
    super()

    this.udx = udx

    this._handle = b4a.allocUnsafe(binding.sizeof_udx_napi_socket_t)
    this._inited = false
    this._host = null
    this._port = 0
    this._reqs = []
    this._free = []
    this._closing = null

    this.streams = new Set()
  }

  static isIPv4 (host) {
    return isIPv4(host)
  }

  get bound () {
    return this._port !== 0
  }

  get closing () {
    return this._closing !== null
  }

  get idle () {
    return this.streams.size === 0
  }

  get busy () {
    return this.streams.size > 0
  }

  _init () {
    if (this._inited) return

    binding.udx_napi_socket_init(this.udx._handle, this._handle, this,
      this._onsend,
      this._onmessage,
      this._onclose
    )

    this._inited = true
  }

  _onsend (id, err) {
    const req = this._reqs[id]

    const onflush = req.onflush

    req.buffer = null
    req.onflush = null

    this._free.push(id)

    onflush(err >= 0)

    // gc the free list
    if (this._free.length >= 16 && this._free.length === this._reqs.length) {
      this._free = []
      this._reqs = []
    }
  }

  _onmessage (buf, port, host) {
    this.emit('message', buf, { host, family: 4, port })
  }

  _onclose () {
    this._handle = null
    this.emit('close')
  }

  _onidle () {
    this.emit('idle')
  }

  _onbusy () {
    this.emit('busy')
  }

  address () {
    if (!this.bound) return null
    return { host: this._host, family: 4, port: this._port }
  }

  bind (port, host) {
    if (this.bound) throw new Error('Already bound')
    if (this.closing) throw new Error('Socket is closed')

    if (!port) port = 0
    if (!host) host = '0.0.0.0'
    if (!isIPv4(host)) throw new Error('Can only bind to IPv4 currently')

    if (!this._inited) this._init()

    this._port = binding.udx_napi_socket_bind(this._handle, port, host)
    this._host = host

    this.emit('listening')
  }

  async close () {
    if (this._closing) return this._closing
    this._closing = new Promise((resolve) => this.once('close', resolve))
    this._closeMaybe()
    return this._closing
  }

  _closeMaybe () {
    if (this._inited && this.closing && this.idle) {
      binding.udx_napi_socket_close(this._handle)
    }
  }

  setTTL (ttl) {
    if (!this._inited) throw new Error('Socket not active')
    binding.udx_napi_socket_set_ttl(this._handle, ttl)
  }

  getRecvBufferSize () {
    if (!this._inited) throw new Error('Socket not active')
    return binding.udx_napi_socket_recv_buffer_size(this._handle, 0)
  }

  setRecvBufferSize (size) {
    if (!this._inited) throw new Error('Socket not active')
    return binding.udx_napi_socket_recv_buffer_size(this._handle, size)
  }

  getSendBufferSize () {
    if (!this._inited) throw new Error('Socket not active')
    return binding.udx_napi_socket_send_buffer_size(this._handle, 0)
  }

  setSendBufferSize (size) {
    if (!this._inited) throw new Error('Socket not active')
    return binding.udx_napi_socket_send_buffer_size(this._handle, size)
  }

  async send (buffer, port, host, ttl) {
    if (this.closing) return false

    if (!host) host = '127.0.0.1'
    if (!this.bound) this.bind(0)

    const id = this._allocSend()
    const req = this._reqs[id]

    req.buffer = buffer

    const promise = new Promise((resolve) => {
      req.onflush = resolve
    })

    binding.udx_napi_socket_send_ttl(this._handle, req.handle, id, buffer, port, host, ttl || 0)

    return promise
  }

  trySend (buffer, port, host, ttl) {
    if (this.closing) return

    if (!host) host = '127.0.0.1'
    if (!this.bound) this.bind(0)

    const id = this._allocSend()
    const req = this._reqs[id]

    req.buffer = buffer
    req.onflush = noop

    binding.udx_napi_socket_send_ttl(this._handle, req.handle, id, buffer, port, host, ttl || 0)
  }

  _allocSend () {
    if (this._free.length > 0) return this._free.pop()
    const handle = b4a.allocUnsafe(binding.sizeof_udx_socket_send_t)
    return this._reqs.push({ handle, buffer: null, onflush: null }) - 1
  }
}

function noop () {}

function isIPv4 (host) {
  return IPv4Pattern.test(host)
}
