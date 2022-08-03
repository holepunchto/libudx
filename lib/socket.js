const events = require('events')
const b4a = require('b4a')
const binding = require('./binding')
const ip = require('./ip')

module.exports = class UDXSocket extends events.EventEmitter {
  constructor (udx) {
    super()

    this.udx = udx

    this._handle = b4a.allocUnsafe(binding.sizeof_udx_napi_socket_t)
    this._inited = false
    this._host = null
    this._family = 0
    this._port = 0
    this._reqs = []
    this._free = []
    this._closing = null
    this._closed = false

    this.streams = new Set()

    this.userData = null
  }

  static isIPv4 (host) {
    return ip.isIPv4(host)
  }

  static isIPv6 (host) {
    return ip.isIPv6(host)
  }

  static isIP (host) {
    return ip.isIP(host)
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

  _onmessage (buf, port, host, family) {
    this.emit('message', buf, { host, family, port })
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

  _addStream (stream) {
    if (this.idle) this._onbusy()

    this.streams.add(stream)
  }

  _removeStream (stream) {
    this.streams.delete(stream)

    const closed = this._closeMaybe()

    if (this.idle && !closed) this._onidle()
  }

  address () {
    if (!this.bound) return null
    return { host: this._host, family: this._family, port: this._port }
  }

  bind (port, host) {
    if (this.bound) throw new Error('Already bound')
    if (this.closing) throw new Error('Socket is closed')

    if (!port) port = 0
    if (!host) host = '0.0.0.0'

    const family = ip.isIP(host)
    if (!family) throw new Error(`${host} is not a valid IP address`)

    if (!this._inited) this._init()

    this._port = binding.udx_napi_socket_bind(this._handle, port, host, family)
    this._host = host
    this._family = family

    this.emit('listening')
  }

  async close () {
    if (this._closing) return this._closing
    this._closing = (this._inited && this.idle) ? events.once(this, 'close') : Promise.resolve(true)
    this._closeMaybe()
    return this._closing
  }

  _closeMaybe () {
    if (!this._closed && this._inited && this.closing && this.idle) {
      binding.udx_napi_socket_close(this._handle)
      this._closed = true
    }

    return this._closed
  }

  setTTL (ttl) {
    if (!this._inited) throw new Error('Socket not active')
    binding.udx_napi_socket_set_ttl(this._handle, ttl)
  }

  getRecvBufferSize () {
    if (!this._inited) throw new Error('Socket not active')
    return binding.udx_napi_socket_get_recv_buffer_size(this._handle)
  }

  setRecvBufferSize (size) {
    if (!this._inited) throw new Error('Socket not active')
    return binding.udx_napi_socket_set_recv_buffer_size(this._handle, size)
  }

  getSendBufferSize () {
    if (!this._inited) throw new Error('Socket not active')
    return binding.udx_napi_socket_get_send_buffer_size(this._handle)
  }

  setSendBufferSize (size) {
    if (!this._inited) throw new Error('Socket not active')
    return binding.udx_napi_socket_set_send_buffer_size(this._handle, size)
  }

  async send (buffer, port, host, ttl) {
    if (this.closing) return false

    if (!host) host = '127.0.0.1'

    const family = ip.isIP(host)
    if (!family) throw new Error(`${host} is not a valid IP address`)

    if (!this.bound) this.bind(0)

    const id = this._allocSend()
    const req = this._reqs[id]

    req.buffer = buffer

    const promise = new Promise((resolve) => {
      req.onflush = resolve
    })

    binding.udx_napi_socket_send_ttl(this._handle, req.handle, id, buffer, port, host, family, ttl || 0)

    return promise
  }

  trySend (buffer, port, host, ttl) {
    if (this.closing) return

    if (!host) host = '127.0.0.1'

    const family = ip.isIP(host)
    if (!family) throw new Error(`${host} is not a valid IP address`)

    if (!this.bound) this.bind(0)

    const id = this._allocSend()
    const req = this._reqs[id]

    req.buffer = buffer
    req.onflush = noop

    binding.udx_napi_socket_send_ttl(this._handle, req.handle, id, buffer, port, host, family, ttl || 0)
  }

  _allocSend () {
    if (this._free.length > 0) return this._free.pop()
    const handle = b4a.allocUnsafe(binding.sizeof_udx_socket_send_t)
    return this._reqs.push({ handle, buffer: null, onflush: null }) - 1
  }
}

function noop () {}
