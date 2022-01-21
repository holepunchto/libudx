const binding = require('./binding')
const UCPStream = require('./stream')
const events = require('events')

const IPv4Pattern = /^((?:[0-9]|[1-9][0-9]|1[0-9][0-9]|2[0-4][0-9]|25[0-5])[.]){3}(?:[0-9]|[1-9][0-9]|1[0-9][0-9]|2[0-4][0-9]|25[0-5])$/

module.exports = class UCP extends events.EventEmitter {
  constructor () {
    super()

    this._handle = Buffer.allocUnsafe(binding.sizeof_ucp_napi_t)
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

  _init () {
    if (this._inited) return

    binding.ucp_napi_init(this._handle, this,
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

  createStream () {
    if (this.closing) throw new Error('Socket is closed')
    if (!this._inited) this._init()
    if (!this.bound) this.bind(0)
    return new UCPStream(this)
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
      this._port = binding.ucp_napi_bind(this._handle, port, ip)
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

    if (this.bound) binding.ucp_napi_close(this._handle)
    else queueMicrotask(() => this.emit('close'))
  }

  _closeMaybe () {
    if (!this.closing || this.streams.size > 0) return
    this.closing = false
    this.close()
  }

  setTTL (ttl) {
    if (!this._inited) throw new Error('Socket not active')
    binding.ucp_napi_set_ttl(this._handle, ttl)
  }

  getRecvBufferSize () {
    if (!this._inited) throw new Error('Socket not active')
    return binding.ucp_napi_recv_buffer_size(this._handle, 0)
  }

  setRecvBufferSize (size) {
    if (!this._inited) throw new Error('Socket not active')
    return binding.ucp_napi_recv_buffer_size(this._handle, size)
  }

  getSendBufferSize () {
    if (!this._inited) throw new Error('Socket not active')
    return binding.ucp_napi_send_buffer_size(this._handle, 0)
  }

  setSendBufferSize (size) {
    if (!this._inited) throw new Error('Socket not active')
    return binding.ucp_napi_send_buffer_size(this._handle, size)
  }

  send (buffer, offset, length, port, ip, onflush) {
    if (this.closing) throw new Error('Socket is closed')
    if (!this.bound) this.bind(0)

    const id = this._allocSend()
    const req = this._reqs[id]

    req.buffer = buffer.subarray(offset, offset + length)
    req.onflush = onflush || noop

    binding.ucp_napi_send(this._handle, req.handle, id, buffer, port, ip)
  }

  _allocSend () {
    if (this._free.length > 0) return this._free.pop()
    const handle = Buffer.allocUnsafe(binding.sizeof_ucp_send_t)
    return this._reqs.push({ handle, buffer: null, onflush: null }) - 1
  }
}

function noop () {}

function isIPv4 (ip) {
  return IPv4Pattern.test(ip)
}
