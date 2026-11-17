# Prints, in order, when the deletion is marked, when the index is queried,
# and when the index is changed. Use with writer_pending_delete.test.
set pagination off
set breakpoint pending on
set $writer = 0

# The deletion is written into storage as a mark carrying the writer's
# transaction ID. IDs of uncommitted transactions are >= 2^63.
break kuzu::storage::VectorVersionInfo::delete_
commands
  silent
  set $writer = transactionID
  printf "[storage] row %lu marked deleted by transaction %lu (uncommitted)\n", rowIdx, transactionID
  continue
end

# Every DYNAMIC_CONNECTIVITY_QUERY ends here. Note: no transaction is passed.
break kuzu::algo_extension::NativeDynamicConnectivityIndex::connected
commands
  silent
  printf "[index]   connected(%lu, %lu) is called\n", src.offset, dst.offset
  continue
end

# Commit of the writer only.
break kuzu::transaction::Transaction::commit if this->ID == $writer
commands
  silent
  printf "[commit]  transaction %lu commits with commit time %lu\n", this->ID, this->commitTS
  continue
end

# Index maintenance.
break kuzu::algo_extension::NativeDynamicConnectivityIndex::commitRelDelete
commands
  silent
  printf "[index]   commitRelDelete(%lu, %lu): the index is changed\n", srcNodeOffset, dstNodeOffset
  continue
end

# The mark is rewritten from the writer's ID to the commit time.
break kuzu::storage::VectorVersionInfo::setDeleteCommitTS
commands
  silent
  printf "[storage] deletion mark rewritten to the commit time\n"
  continue
end

run

