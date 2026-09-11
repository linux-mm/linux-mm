.. SPDX-License-Identifier: GPL-2.0

KCOV-Dataflow Selftests: rust_kworker_remote
============================================

Rust module testing kcov_df_remote_start()/kcov_df_remote_stop() from
kworker context: the trigger queues a work item on system_wq whose three
phases (populate/update/drain of a CompositeStore of RBTrees) run with
remote capture on handle 1, which the runner publishes with
KCOV_DF_REMOTE_ENABLE. Built only with CONFIG_RUST=y::

  ./test_modules.py -t rust_kworker_remote
  ./trigger-view.py rust_kworker_remote --remote
