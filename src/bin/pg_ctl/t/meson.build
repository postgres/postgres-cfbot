# Copyright (c) 2026, PostgreSQL Global Development Group

# Keep the test helper out of pg_ctl's build directory. Meson adds test
# dependency directories to PATH, and the uninstalled pg_ctl cannot find
# its sibling postgres executable there.
socket_test_helper = executable('pg_ctl_socket_helper',
  'socket_copy_helper.c',
  dependencies: cc.find_library('ws2_32'),
  install: false,
)
