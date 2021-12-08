{
  "targets": [{
    "target_name": "ucp",
    "include_dirs": [
      "<!(node -e \"require('napi-macros')\")",
    ],
    "sources": [
      "./src/cirbuf.c",
      "./src/fifo.c",
      "./src/ucp.c",
      "./src/utils.c",
      "./binding.c",
    ],
    'xcode_settings': {
      'OTHER_CFLAGS': [
        '-O3',
      ]
    },
    'cflags': [
      '-O3',
    ],
  }]
}
