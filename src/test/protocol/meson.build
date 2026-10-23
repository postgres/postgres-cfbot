# Copyright (c) 2022-2026, PostgreSQL Global Development Group

tests += {
  'name': 'protocol',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'env': {
      'with_ssl': ssl_library,
      'OPENSSL': openssl.found() ? openssl.full_path() : '',
    },
    'tests': [
      't/001_proxy_protocol.pl',
      't/002_proxy_protocol_ssl.pl',
    ],
  },
}
