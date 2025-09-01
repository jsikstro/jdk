/*
 * Copyright (c) 2024, 2025, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

#ifndef SHARE_GC_Z_ZMEMORYWORKER_HPP
#define SHARE_GC_Z_ZMEMORYWORKER_HPP

#include "gc/z/zAdaptiveHeap.hpp"
#include "gc/z/zAddress.hpp"
#include "gc/z/zArray.hpp"
#include "gc/z/zList.hpp"
#include "gc/z/zLock.hpp"
#include "gc/z/zThread.hpp"
#include "gc/z/zVirtualMemory.hpp"
#include "memory/allocation.hpp"
#include "utilities/ticks.hpp"
#include "utilities/rbTree.hpp"

class ZPartition;

class ZHeatingRequestTreeComparator : public AllStatic {
public:
  static int cmp(zoffset first, zoffset second);
};

using ZHeatingRequestTree = RBTreeCHeap<zoffset, size_t, ZHeatingRequestTreeComparator, mtGC>;
using ZHeatingRequestNode = RBNode<zoffset, size_t>;

// This worker is enabled with automatic heap sizing.
// Its responsibilities are:
// * Change heap capacity
//   - Commit memory concurrently when growing the heap
//   - Uncommit memory concurrently due to long-term shrinking
//   - Uncommit memory concurrently due to urgent memory pressure
// * Heat up memory before mutators start using it
//   - Pre-touching memory concurrently
//   - Collapse small pages to large pages
// There is one memory worker per heap partition

class ZMemoryWorker : public ZThread {
private:
  const uint32_t      _id;
  ZPartition* const   _partition;
  ZConditionLock      _lock;
  ZHeatingRequestTree _heating_requests;
  size_t              _enqueued_heating;
  volatile size_t     _target_capacity;
  bool                _stop;
  ZVirtualMemory      _currently_heating;

  bool is_stop_requested();
  size_t commit_granule(size_t capacity, size_t target_capacity);
  size_t uncommit_granule();
  bool should_commit(size_t granule, size_t capacity, size_t target_capacity, size_t curr_max_capacity, const ZMemoryPressureMetrics& metrics);
  bool should_uncommit(size_t granule, size_t capacity, size_t target_capacity);

  bool throttle_uncommit(Ticks start);
  size_t uncommit(size_t to_uncommit);

  void assert_enqueued_size();
  void assert_not_tracked(const ZVirtualMemory& vmem);
  void assert_not_tracked(zoffset start, size_t size);
  bool should_heat();
  bool has_heating_request();
  void remove_heating_request_range(const ZVirtualMemory& vmem);
  ZVirtualMemory pop_heating_request();
  size_t process_heating_request();
  bool peek();

protected:
  virtual void run_thread();
  virtual void terminate();

public:
  ZMemoryWorker(uint32_t id, ZPartition* partition);

  void heap_resized(size_t capacity, size_t heuristic_max_capacity);
  void heap_truncated(size_t capacity);
  void grow_target_capacity(size_t target_capacity);
  void shrink_target_capacity(size_t target_capacity);
  void critical_shrink_target_capacity();
  void set_target_capacity(size_t target_capacity);
  size_t target_capacity();

  void register_heating_request(const ZVirtualMemory& vmem);
  void remove_heating_request(const ZVirtualMemory& vmem);
};

#endif // SHARE_GC_Z_ZMEMORYWORKER_HPP
