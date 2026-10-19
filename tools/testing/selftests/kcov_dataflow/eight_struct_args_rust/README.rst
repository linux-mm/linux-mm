.. SPDX-License-Identifier: GPL-2.0

KCOV-Dataflow Selftests: eight_struct_args_rust
===============================================

Rust equivalent of eight_struct_args_c (rsf_*, rstf_*, rstpf_* with
``#[no_mangle]``), built only with CONFIG_RUST=y. Opted in with
``KCOV_DATAFLOW_eight_struct_args_rust.o := y``::

  ./test_modules.py -t eight_struct_args_rust
  ./trigger-view.py eight_struct_args_rust --raw
