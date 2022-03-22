{
  'targets': [{
    'target_name': 'udx',
    'include_dirs': [
      '<!(node -e "require(\'napi-macros\')")',
    ],
    'sources': [
      './src/cirbuf.c',
      './src/fifo.c',
      './src/udx.c',
      './src/io_posix.c',
      './binding.c',
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
