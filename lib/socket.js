const binding = require('./binding')
const UDXStream = require('./stream')
const events = require('events')

const IPv4Pattern = /^((?:[0-9]|[1-9][0-9]|1[0-9][0-9]|2[0-4][0-9]|25[0-5])[.]){3}(?:[0-9]|[1-9][0-9]|1[0-9][0-9]|2[0-4][0-9]|25[0-5])$/

module.exports = class UDXSocket extends events.EventEmitter {
  constructor (udxHandle) {
    super()

    this._udxHandle = udxHandle
    this._handle = Buffer.allocUnsafe(binding.sizeof_udx_napi_socket_t)
    this._inited = false
    this._ip = null
    this._port = 0
    this._reqs = []
    this._free = []

    this.streams = new Set()
    this.bound = false
    this.closing = false
  }

  static isIPv4 (ip) {
    return isIPv4(ip)
  }

  get idle () {
    return this.streams.size === 0
  }

  get busy () {
    return this.streams.size > 0
  }

  _init () {
    if (this._inited) return

    binding.udx_napi_socket_init(this._udxHandle, this._handle, this,
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

    onflush(err < 0 ? new Error('Send failed') : null)

    // gc the free list
    if (this._free.length >= 16 && this._free.length === this._reqs.length) {
      this._free = []
      this._reqs = []
    }
  }

  _onmessage (buf, port, ip) {
    this.emit('message', buf, { address: ip, family: 'IPv4', port })
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
    return { address: this._ip, family: 'IPv4', port: this._port }
  }

  bind (port, ip, onlistening) {
    if (typeof port === 'function') return this.bind(0, null, onlistening)
    if (typeof ip === 'function') return this.bind(port, null, ip)
    if (this.bound) throw new Error('Already bound')
    if (this.closing) throw new Error('Socket is closed')
    if (!port) port = 0
    if (!ip) ip = '0.0.0.0'
    if (!isIPv4(ip)) throw new Error('Can only bind to IPv4 currently')

    if (onlistening) this.once('listening', onlistening)

    if (!this._inited) this._init()

    try {
      this._port = binding.udx_napi_socket_bind(this._handle, port, ip)
      this._ip = ip
    } catch (err) {
      this.emit('error', err)
      return
    }

    this.bound = true

    // nextTicked here for nodejs compat
    queueMicrotask(() => { if (this.bound) this.emit('listening') })
  }

  close (onclose) {
    if (this.closing) return
    this.closing = true

    if (onclose) this.once('close', onclose)

    if (this.streams.size) return

    if (this._inited) binding.udx_napi_socket_close(this._handle)
    else queueMicrotask(() => this.emit('close'))
  }

  _closeMaybe () {
    if (!this.closing || this.streams.size > 0) return
    this.closing = false
    this.close()
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

  send (buffer, offset, length, port, ip, ttl, onflush) {
    if (this.closing) throw new Error('Socket is closed')
    if (!this.bound) this.bind(0)

    if (typeof ttl === 'function') {
      onflush = ttl
      ttl = 0
    }

    const id = this._allocSend()
    const req = this._reqs[id]

    req.buffer = buffer.subarray(offset, offset + length)
    req.onflush = onflush || noop

    binding.udx_napi_socket_send_ttl(this._handle, req.handle, id, buffer, port, ip, ttl || 0)
  }

  _allocSend () {
    if (this._free.length > 0) return this._free.pop()
    const handle = Buffer.allocUnsafe(binding.sizeof_udx_socket_send_t)
    return this._reqs.push({ handle, buffer: null, onflush: null }) - 1
  }

  static createSocket () {
    return new this()
  }

  static createStream (id) {
    return new UDXStream(id)
  }
}

function noop () {}

function isIPv4 (ip) {
  return IPv4Pattern.test(ip)
}
