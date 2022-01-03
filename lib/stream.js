const binding = require('./binding')

const MTU = 2048
const BUFFER_SIZE = 65536 + MTU

module.exports = class UCPStream {
  constructor (socket) {
    this._socket = socket
    this._handle = Buffer.allocUnsafe(binding.sizeof_ucp_napi_stream_t)
    this._reqs = []
    this._free = []
    this._readBuffer = null

    this.onread = noop
    this.onend = noop
    this.ondrain = noop
    this.onclose = noop

    this.connected = false
    this.remoteId = 0
    this.id = binding.ucp_napi_stream_init(socket._handle, this._handle, this,
      this._onread,
      this._onend,
      this._ondrain,
      this._onack,
      this._onclose,
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
    if (this.connected) throw new Error('Already connected')
    if (!host) host = '127.0.0.1'
    this.remoteId = remoteId
    this.connected = true
    this._readBuffer = Buffer.allocUnsafe(BUFFER_SIZE)
    binding.ucp_napi_stream_connect(this._handle, this._readBuffer, remoteId, port, host)
  }

  write (buffer) {
    const id = this._allocWrite()
    const req = this._reqs[id]

    req.buffer = buffer

    return binding.ucp_napi_stream_write(this._handle, req.handle, id, buffer) !== 0
  }

  end () {
    const id = this._allocWrite()
    const req = this._reqs[id]

    req.buffer = null

    return binding.ucp_napi_stream_end(this._handle, req.handle, id) !== 0
  }

  _onread (read) {
    const data = this._readBuffer.subarray(0, read)

    this.onread(data)

    this._readBuffer = this._readBuffer.byteLength - read > MTU
      ? this._readBuffer.subarray(read)
      : Buffer.allocUnsafe(BUFFER_SIZE)

    return this._readBuffer
  }

  _onend () {
    this.onend()
  }

  _ondrain () {
    this.ondrain()
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

  _onclose (hard) {
    this.onclose(hard)
  }

  _allocWrite () {
    if (this._free.length > 0) return this._free.pop()
    const handle = Buffer.allocUnsafe(binding.sizeof_ucp_write_t)
    return this._reqs.push({ handle, buffer: null }) - 1
  }
}

function noop () {}
