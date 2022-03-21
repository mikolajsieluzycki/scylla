nnoremap <F7> :AsyncRun ninja build/dev/test/boost/repair_test<CR>
nnoremap <F6> :AsyncRun build/dev/test/boost/repair_test -- -c2<CR>
"nnoremap <F6> :AsyncRun build/dev/test/boost/multishard_combining_reader_as_mutation_source_test --run_test=test_partition_version_consistency_after_lsa_compaction_happens -- -c2<CR>
