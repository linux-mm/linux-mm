.. SPDX-License-Identifier: GPL-2.0

KCOV-Dataflow Selftests: eight_struct_args_c
============================================

C module with 1-8 struct pointer arguments (flat s1..s8), value-nested
st1..st8 and pointer-linked stp1..stp8 towers (on stack, kmalloc and
vmalloc), pointer forwarding and a struct return value. Opted in with
``KCOV_DATAFLOW_eight_struct_args_c.o := y``; test_modules.py checks the
expanded fields (0x11, 0x22, ...) and every return value::

  ./test_modules.py -t eight_struct_args_c
  ./trigger-view.py eight_struct_args_c --raw
