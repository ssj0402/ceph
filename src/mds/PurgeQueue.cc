// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2015 Red Hat
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

#include "common/debug.h"
#include "mds/mdstypes.h"
#include "mds/CInode.h"

#include "PurgeQueue.h"


#define dout_context cct
#define dout_subsys ceph_subsys_mds
#undef dout_prefix
#define dout_prefix _prefix(_dout, rank) << __func__ << ": "
static ostream& _prefix(std::ostream *_dout, mds_rank_t rank) {
  return *_dout << "mds." << rank << ".purge_queue ";
}

void PurgeItem::encode(bufferlist &bl) const
{
  ENCODE_START(1, 1, bl);
  ::encode(ino, bl);
  ::encode(size, bl);
  ::encode(layout, bl, CEPH_FEATURE_FS_FILE_LAYOUT_V2);
  ::encode(old_pools, bl);
  ::encode(snapc, bl);
  ENCODE_FINISH(bl);
}

void PurgeItem::decode(bufferlist::iterator &p)
{
  DECODE_START(1, p);
  ::decode(ino, p);
  ::decode(size, p);
  ::decode(layout, p);
  ::decode(old_pools, p);
  ::decode(snapc, p);
  DECODE_FINISH(p);
}

// TODO: implement purge queue creation on startup
// if we are on a filesystem created before purge queues existed
// TODO: ensure that a deactivating MDS rank blocks
// on complete drain of this queue before finishing
// TODO: when we're deactivating, lift all limits on
// how many OSD ops we're allowed to emit at a time to
// race through the queue as fast as we can.
// TODO: populate logger here to gather latency stat?
//       ...and a stat for the size of the queue, if we can
//       somehow track that?  Could do an initial pass through
//       the whole queue to count the items at startup?
// TODO: there is absolutely no reason to consume an inode number
// for this.  Shoudl just give objects a string name with a rank
// suffix, like we do for MDSTables.  Requires a little refactor
// of Journaler.
PurgeQueue::PurgeQueue(
      CephContext *cct_,
      mds_rank_t rank_,
      const int64_t metadata_pool_,
      Objecter *objecter_)
  :
    cct(cct_),
    rank(rank_),
    lock("PurgeQueue"),
    metadata_pool(metadata_pool_),
    finisher(cct, "PurgeQueue", "PQ_Finisher"),
    timer(cct, lock),
    filer(objecter_, &finisher),
    objecter(objecter_),
    journaler("pq", MDS_INO_PURGE_QUEUE + rank, metadata_pool,
      CEPH_FS_ONDISK_MAGIC, objecter_, nullptr, 0, &timer,
      &finisher)
{
}

void PurgeQueue::init()
{
  Mutex::Locker l(lock);

  finisher.start();
  timer.init();
}

void PurgeQueue::shutdown()
{
  Mutex::Locker l(lock);

  journaler.shutdown();
  timer.shutdown();
  finisher.stop();
}

void PurgeQueue::open(Context *completion)
{
  dout(4) << "opening" << dendl;

  Mutex::Locker l(lock);

  journaler.recover(new FunctionContext([this, completion](int r){
    Mutex::Locker l(lock);
    dout(4) << "open complete" << dendl;
    if (r == 0) {
      journaler.set_writeable();
    }
    completion->complete(r);
  }));
}

void PurgeQueue::create(Context *fin)
{
  dout(4) << "creating" << dendl;
  Mutex::Locker l(lock);

  file_layout_t layout = file_layout_t::get_default();
  layout.pool_id = metadata_pool;
  journaler.set_writeable();
  journaler.create(&layout, JOURNAL_FORMAT_RESILIENT);
  journaler.write_head(fin);
}

void PurgeQueue::push(const PurgeItem &pi, Context *completion)
{
  dout(4) << "pushing inode 0x" << std::hex << pi.ino << std::dec << dendl;
  Mutex::Locker l(lock);

  // Callers should have waited for open() before using us
  assert(!journaler.is_readonly());

  bufferlist bl;

  ::encode(pi, bl);
  journaler.append_entry(bl);

  // Note that flush calls are not 1:1 with IOs, Journaler
  // does its own batching.  So we just call every time.
  // FIXME: *actually* as soon as we call _consume it will
  // do a flush via _issue_read, so we really are doing one
  // write per event.  Avoid this by avoiding doing the journaler
  // read (see "if we could consume this PurgeItem immediately...")
  journaler.flush(completion);

  // Maybe go ahead and do something with it right away
  _consume();

  // TODO: if we could consume this PurgeItem immediately, and
  // Journaler does not have any outstanding prefetches, then
  // short circuit its read by advancing read_pos to write_pos
  // and passing the PurgeItem straight into _execute_item
}

bool PurgeQueue::can_consume()
{
  // TODO: enforce limits (currently just allowing one in flight)
  if (in_flight.size() > 0) {
    return false;
  } else {
    return true;
  }
}

void PurgeQueue::_consume()
{
  assert(lock.is_locked_by_me());

  // Because we are the writer and the reader of the journal
  // via the same Journaler instance, we never need to reread_head
  
  if (!can_consume()) {
    dout(10) << " cannot consume right now" << dendl;

    return;
  }

  if (!journaler.is_readable()) {
    dout(10) << " not readable right now" << dendl;
    if (!journaler.have_waiter()) {
      journaler.wait_for_readable(new FunctionContext([this](int r) {
        Mutex::Locker l(lock);
        if (r == 0) {
          _consume();
        }
      }));
    }

    return;
  }

  // The journaler is readable: consume an entry
  bufferlist bl;
  bool readable = journaler.try_read_entry(bl);
  assert(readable);  // we checked earlier

  dout(20) << " decoding entry" << dendl;
  PurgeItem item;
  bufferlist::iterator q = bl.begin();
  ::decode(item, q);
  dout(20) << " executing item (0x" << std::hex << item.ino
           << std::dec << ")" << dendl;
  _execute_item(item, journaler.get_read_pos());
}

void PurgeQueue::_execute_item(
    const PurgeItem &item,
    uint64_t expire_to)
{
  assert(lock.is_locked_by_me());

  in_flight[expire_to] = item;

  // TODO: handle things other than normal file purges
  // (directories, snapshot truncates)
  C_GatherBuilder gather(cct);
  if (item.size > 0) {
    uint64_t num = Striper::get_num_objects(item.layout, item.size);
    dout(10) << " 0~" << item.size << " objects 0~" << num
             << " snapc " << item.snapc << " on " << item.ino << dendl;
    filer.purge_range(item.ino, &item.layout, item.snapc,
                      0, num, ceph::real_clock::now(), 0,
                      gather.new_sub());
  }

  // remove the backtrace object if it was not purged
  object_t oid = CInode::get_object_name(item.ino, frag_t(), "");
  if (!gather.has_subs() || !item.layout.pool_ns.empty()) {
    object_locator_t oloc(item.layout.pool_id);
    dout(10) << " remove backtrace object " << oid
	     << " pool " << oloc.pool << " snapc " << item.snapc << dendl;
    objecter->remove(oid, oloc, item.snapc,
			  ceph::real_clock::now(), 0,
			  NULL, gather.new_sub());
  }

  // remove old backtrace objects
  for (const auto &p : item.old_pools) {
    object_locator_t oloc(p);
    dout(10) << " remove backtrace object " << oid
	     << " old pool " << p << " snapc " << item.snapc << dendl;
    objecter->remove(oid, oloc, item.snapc,
			  ceph::real_clock::now(), 0,
			  NULL, gather.new_sub());
  }
  assert(gather.has_subs());

  gather.set_finisher(new FunctionContext([this, expire_to](int r){
    execute_item_complete(expire_to);
  }));
  gather.activate();
}

void PurgeQueue::execute_item_complete(
    uint64_t expire_to)
{
  dout(10) << "complete at 0x" << std::hex << expire_to << std::dec << dendl;
  Mutex::Locker l(lock);
  assert(in_flight.count(expire_to) == 1);

  auto iter = in_flight.find(expire_to);
  assert(iter != in_flight.end());
  if (iter == in_flight.begin()) {
    // This was the lowest journal position in flight, so we can now
    // safely expire the journal up to here.
    journaler.set_expire_pos(expire_to);
    journaler.trim();
  }

  dout(10) << "completed item for ino 0x" << std::hex << iter->second.ino
           << std::dec << dendl;

  in_flight.erase(iter);

  _consume();
}

