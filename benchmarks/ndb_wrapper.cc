#ifndef _NDB_WRAPPER_IMPL_H_
#define _NDB_WRAPPER_IMPL_H_

#include <stdint.h>
#include "ndb_wrapper.h"
#include "../dbcore/rcu.h"
#include "../varkey.h"
#include "../macros.h"
#include "../util.h"
#include "../txn.h"
#include "../tuple.h"

ndb_wrapper::ndb_wrapper()
{
  ALWAYS_ASSERT(sysconf::log_dir.size());
  INVARIANT(!logmgr);
  INVARIANT(!oidmgr);

  RCU::rcu_register();
  RCU::rcu_enter();
  logmgr = sm_log::new_log(sm_log::recover, NULL);
  ASSERT(oidmgr);
  RCU::rcu_exit();
  // rcu_deregister in dtor
}

ndb_wrapper::~ndb_wrapper()
{
  logmgr->flush();
  RCU::rcu_deregister();
}

size_t
ndb_wrapper::sizeof_txn_object(uint64_t txn_flags) const
{
  return sizeof(transaction);
}

void *
ndb_wrapper::new_txn(
    uint64_t txn_flags,
    str_arena &arena,
    void *buf,
    TxnProfileHint hint)
{
  ndbtxn * const p = reinterpret_cast<ndbtxn *>(buf);
  p->hint = hint;
  new (&p->buf[0]) transaction(txn_flags, arena);
  return p;
}

rc_t
ndb_wrapper::commit_txn(void *txn)
{
  ndbtxn * const p = reinterpret_cast<ndbtxn *>(txn);
  auto t = (transaction *)&p->buf[0];
  rc_t rc = t->commit();
  if (not rc_is_abort(rc))
    t->~transaction();
  return rc;
}

void
ndb_wrapper::abort_txn(void *txn)
{
  ndbtxn * const p = reinterpret_cast<ndbtxn *>(txn);
  auto t = (transaction *)&p->buf[0];
  t->abort();
  t->~transaction();
}

void
ndb_wrapper::print_txn_debug(void *txn) const
{
  ndbtxn * const p = reinterpret_cast<ndbtxn *>(txn);
  auto t = (transaction *)&p->buf[0];
  t->dump_debug_info(); \
}

abstract_ordered_index *
ndb_wrapper::open_index(const std::string &name, size_t value_size_hint, bool mostly_append)
{
  FID fid = 0;

  // See if we already have an FID for this table (recovery did it)
  std::unordered_map<std::string, std::pair<FID, ndb_ordered_index *> >::const_iterator it = fid_map.find(name);
  if (it != fid_map.end())
      fid = it->second.first;
  else {
      fid = oidmgr->create_file(true);
      // log [table name, FID]
      RCU::rcu_enter();
      ASSERT(logmgr);
      sm_tx_log *log = logmgr->new_tx_log();
      log->log_fid(fid, name);
      log->commit(NULL);
      RCU::rcu_exit();
  }
  ASSERT(fid);
  auto *index = new ndb_ordered_index(name, fid, value_size_hint, mostly_append);
  fid_map.emplace(name, std::make_pair(fid, index));
  // Prepare the index pointer for rebuild
  if (it != fid_map.end()) {
      ASSERT(it->second.second == NULL);
      fid_map[name].second = index;
  }
  return index;
}

void
ndb_wrapper::close_index(abstract_ordered_index *idx)
{
  delete idx;
}

ndb_ordered_index::ndb_ordered_index(
    const std::string &name, FID fid, size_t value_size_hint, bool mostly_append)
  : name(name), btr(value_size_hint, mostly_append, name, fid)
{
  // for debugging
  //std::cerr << name << " : btree= "
  //          << btr.get_underlying_btree()
  //          << std::endl;
}

rc_t
ndb_ordered_index::get(
    void *txn,
    const varstr &key,
    varstr &value, size_t max_bytes_read)
{
  ndbtxn * const p = reinterpret_cast<ndbtxn *>(txn);
  auto t = (transaction *)&p->buf[0];
  return btr.search(*t, key, value, max_bytes_read);
}

rc_t
ndb_ordered_index::put(
    void *txn,
    const varstr &key,
    const varstr &value)
{
  ndbtxn * const p = reinterpret_cast<ndbtxn *>(txn);
  auto t = (transaction *)&p->buf[0];
  return btr.put(*t, key, value);
}

rc_t
ndb_ordered_index::put(
    void *txn,
    varstr &&key,
    varstr &&value)
{
  ndbtxn * const p = reinterpret_cast<ndbtxn *>(txn);
  auto t = (transaction *)&p->buf[0];
  return btr.put(*t, std::move(key), std::move(value));
}

rc_t
ndb_ordered_index::insert(
    void *txn,
    const varstr &key,
    const varstr &value)
{
  ndbtxn * const p = reinterpret_cast<ndbtxn *>(txn);
  auto t = (transaction *)&p->buf[0];
  return btr.insert(*t, key, value);
}

rc_t
ndb_ordered_index::insert(
    void *txn,
    varstr &&key,
    varstr &&value)
{
  ndbtxn * const p = reinterpret_cast<ndbtxn *>(txn);
  auto t = (transaction *)&p->buf[0];
  return btr.insert(*t, std::move(key), std::move(value));
}

class ndb_wrapper_search_range_callback : public txn_btree::search_range_callback {
public:
  ndb_wrapper_search_range_callback(abstract_ordered_index::scan_callback &upcall)
    : txn_btree::search_range_callback(), upcall(&upcall) {}

  virtual bool
  invoke(const txn_btree::keystring_type &k,
         const varstr &v)
  {
    return upcall->invoke(k.data(), k.length(), v);
  }

private:
  abstract_ordered_index::scan_callback *upcall;
};

rc_t
ndb_ordered_index::scan(
    void *txn,
    const varstr &start_key,
    const varstr *end_key,
    scan_callback &callback,
    str_arena *arena)
{
  ndbtxn * const p = reinterpret_cast<ndbtxn *>(txn);
  auto t = (transaction *)&p->buf[0];
  ndb_wrapper_search_range_callback c(callback);
  ASSERT(c.return_code._val == RC_FALSE);
  btr.search_range_call(*t, start_key, end_key, c);
  return c.return_code;
}

rc_t
ndb_ordered_index::rscan(
    void *txn,
    const varstr &start_key,
    const varstr *end_key,
    scan_callback &callback,
    str_arena *arena)
{
  ndbtxn * const p = reinterpret_cast<ndbtxn *>(txn);
  ndb_wrapper_search_range_callback c(callback);
  auto t = (transaction *)&p->buf[0];
  ASSERT(c.return_code._val == RC_FALSE);
  btr.rsearch_range_call(*t, start_key, end_key, c);
  return c.return_code;
}

rc_t
ndb_ordered_index::remove(void *txn, const varstr &key)
{
  ndbtxn * const p = reinterpret_cast<ndbtxn *>(txn);
  auto t = (transaction *)&p->buf[0];
  return btr.remove(*t, key);
}

rc_t
ndb_ordered_index::remove(void *txn, varstr &&key)
{
  ndbtxn * const p = reinterpret_cast<ndbtxn *>(txn);
  auto t = (transaction *)&p->buf[0];
  return btr.remove(*t, std::move(key));
}

size_t
ndb_ordered_index::size() const
{
  return btr.size_estimate();
}

std::map<std::string, uint64_t>
ndb_ordered_index::clear()
{
#ifdef TXN_BTREE_DUMP_PURGE_STATS
  std::cerr << "purging txn index: " << name << std::endl;
#endif
  return btr.unsafe_purge(true);
}

#endif /* _NDB_WRAPPER_IMPL_H_ */
