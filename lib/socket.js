const binding = require('./binding')
const UCPStream = require('./stream')

module.exports = class UCP {
  constructor () {
    this._handle = Buffer.allocUnsafe(binding.sizeof_ucp_napi_t)
    this._inited = false
    this._ip = null
    this._port = 0
    this._reqs = []
    this._free = []

    this.bound = false
    this.onmessage = noop
    this.onclose = noop
  }

  _init () {
    binding.ucp_napi_init(this._handle, this,
      this._onsend,
      this._onmessage,
      this._onclose
    )

    this._inited = true
  }

  _onsend (id) {
    const req = this._reqs[id]

    const onflush = req.onflush
    req.buffer = null
    req.onflush = null
    this._free.push(id)

    onflush(null)

    // gc the free list
    if (this._free.length >= 16 && this._free.length === this._reqs.length) {
      this._free = []
      this._reqs = []
    }
  }

  _onmessage (buf, port, ip) {
    this.onmessage(buf, port, ip)
  }

  _onclose () {
    this.onclose()
  }

  createStream () {
    if (!this._inited) this._init()
    return new UCPStream(this)
  }

  address () {
    if (!this.bound) return null
    return { address: this._ip, family: 'IPv4', port: this._port }
  }

  bind (port, ip) {
    if (this.bound) throw new Error('Already bound')
    if (!port) port = 0
    if (!ip) ip = '0.0.0.0'
    if (!this._inited) this._init()
    this._ip = ip
    this._port = binding.ucp_napi_bind(this._handle, port, ip)
    this.bound = true

    return this._port
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

  send (buffer, port, ip, onflush) {
    if (!this.bound) this.bind(0)

    const id = this._allocSend()
    const req = this._reqs[id]

    req.buffer = buffer
    req.onflush = onflush || noop

    binding.ucp_napi_send(this._handle, req.handle, id, buffer, port, ip)
  }

  close () {
    binding.ucp_napi_close(this._handle)
  }

  _allocSend () {
    if (this._free.length > 0) return this._free.pop()
    const handle = Buffer.allocUnsafe(binding.sizeof_ucp_send_t)
    return this._reqs.push({ handle, buffer: null, onflush: null }) - 1
  }
}

function noop () {}
