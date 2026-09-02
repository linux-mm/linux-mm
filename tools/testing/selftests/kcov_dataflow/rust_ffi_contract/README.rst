.. SPDX-License-Identifier: GPL-2.0

KCOV-Dataflow Selftests: rust_ffi_contract
==========================================

FFI contract violation detection: ffi_alloc_buf() returns 0 but leaves
alloc->buffer NULL, and ffi_check_result() receives that NULL. The test
checks the expanded ``struct ffi_alloc`` at both boundaries, the scalar
arguments (256, 16, 1), the 0 return and the -EFAULT from the checker.
Opted in with ``KCOV_DATAFLOW_rust_ffi_contract.o := y``::

  ./test_modules.py -t rust_ffi_contract
  ./trigger-view.py rust_ffi_contract -C 8
