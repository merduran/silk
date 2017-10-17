{
  "targets": [
    {
      "target_name": "silk-alog",
      "sources": [
        "bindings.cpp",
      ],
      "include_dirs": [
        "<!(node -e \"require('nan')\")",
      ],
      "conditions": [
        [
          "OS == 'android'", {
            "libraries" : [
              "<!(echo $Android_mk__LIBRARIES)",
            ],
            "cflags" : [
              "-DANDROID",
              "-DUSE_LIBLOG",
            ],
          },
          'OS == "linux"', {
            'libraries': [
              '<!(test -z "$USE_LIBLOG" || echo "-llog")',
            ],
            'cflags': [
              '<!(test -z "$USE_LIBLOG" || echo "-DUSE_LIBLOG")',
            ],
          },
        ],
      ],
    }
  ]
}
