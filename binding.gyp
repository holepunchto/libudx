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
      './binding.c',
    ],
    'configurations': {
      'Debug': {
        'defines': ['DEBUG'],
      },
      'Release': {
        'defines': ['NDEBUG'],
      },
    },
    'conditions': [
      ['OS=="win"', {
        'sources': [
          './src/io_win.c',
        ],
        'libraries': [
          '-lws2_32',
        ]
      }, {
        'sources': [
          './src/io_posix.c',
        ],
      }],
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
