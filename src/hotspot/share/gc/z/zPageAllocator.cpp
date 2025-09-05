/*
 * Copyright (c) 2015, 2025, Oracle and/or its affiliates. All rights reserved.
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

#include "gc/shared/gcLogPrecious.hpp"
#include "gc/shared/gc_globals.hpp"
#include "gc/shared/suspendibleThreadSet.hpp"
#include "gc/z/zAdaptiveHeap.hpp"
#include "gc/z/zAddress.hpp"
#include "gc/z/zAllocationFlags.hpp"
#include "gc/z/zArray.inline.hpp"
#include "gc/z/zDriver.hpp"
#include "gc/z/zFuture.inline.hpp"
#include "gc/z/zGeneration.inline.hpp"
#include "gc/z/zGenerationId.hpp"
#include "gc/z/zGlobals.hpp"
#include "gc/z/zLargePages.inline.hpp"
#include "gc/z/zLock.inline.hpp"
#include "gc/z/zMappedCache.inline.hpp"
#include "gc/z/zMemoryWorker.hpp"
#include "gc/z/zNUMA.inline.hpp"
#include "gc/z/zPage.inline.hpp"
#include "gc/z/zPageAge.hpp"
#include "gc/z/zPageAllocator.inline.hpp"
#include "gc/z/zPageType.hpp"
#include "gc/z/zPhysicalMemoryManager.hpp"
#include "gc/z/zSafeDelete.inline.hpp"
#include "gc/z/zStat.hpp"
#include "gc/z/zTask.hpp"
#include "gc/z/zUncommitter.hpp"
#include "gc/z/zValue.inline.hpp"
#include "gc/z/zVirtualMemory.inline.hpp"
#include "gc/z/zVirtualMemoryManager.inline.hpp"
#include "gc/z/zWorkers.hpp"
#include "jfr/jfrEvents.hpp"
#include "logging/log.hpp"
#include "memory/allocation.hpp"
#include "nmt/memTag.hpp"
#include "runtime/globals.hpp"
#include "runtime/init.hpp"
#include "runtime/java.hpp"
#include "runtime/os.hpp"
#include "utilities/align.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/powerOfTwo.hpp"
#include "utilities/ticks.hpp"
#include "utilities/vmError.hpp"

#include <cmath>

class ZMemoryAllocation;

static const ZStatCounter       ZCounterMutatorAllocationRate("Memory", "Allocation Rate", ZStatUnitBytesPerSecond);
static const ZStatCounter       ZCounterMappedCacheHarvest("Memory", "Mapped Cache Harvest", ZStatUnitBytesPerSecond);
static const ZStatCounter       ZCounterDefragment("Memory", "Defragment", ZStatUnitOpsPerSecond);
static const ZStatCriticalPhase ZCriticalPhaseAllocationStall("Allocation Stall");

static void check_numa_mismatch(const ZVirtualMemory& vmem, uint32_t desired_id) {
  if (ZNUMA::is_enabled()) {
    // Check if memory ended up on desired NUMA node or not
    const uint32_t actual_id = ZNUMA::memory_id(untype(ZOffset::address(vmem.start())));
    if (actual_id != desired_id) {
      log_debug(gc, heap)("NUMA Mismatch: desired %d, actual %d", desired_id, actual_id);
    }
  }
}

enum class ZPageAllocationAttempt {
  initial,
  retry,
  stall,
};

class ZMemoryAllocation : public CHeapObj<mtGC> {
private:
  const size_t           _size;
  ZPartition*            _partition;
  ZVirtualMemory         _satisfied_from_cache_vmem;
  ZArray<ZVirtualMemory> _partial_vmems;
  int                    _num_harvested;
  size_t                 _harvested;
  size_t                 _increased_capacity;
  size_t                 _committed_capacity;
  bool                   _commit_failed;

  explicit ZMemoryAllocation(const ZMemoryAllocation& other)
    : ZMemoryAllocation(other._size) {
    // Transfer the partition
    set_partition(other._partition);

    // Reserve space for the partial vmems
    _partial_vmems.reserve(other._partial_vmems.length() + (other._satisfied_from_cache_vmem.is_null() ? 1 : 0));

    // Transfer the claimed capacity
    transfer_claimed_capacity(other);
  }

  ZMemoryAllocation(const ZMemoryAllocation& a1, const ZMemoryAllocation& a2)
    : ZMemoryAllocation(a1._size + a2._size) {
    // Transfer the partition
    assert(a1._partition == a2._partition, "only merge with same partition");
    set_partition(a1._partition);

    // Reserve space for the partial vmems
    const int num_vmems_a1 = a1._partial_vmems.length() + (a1._satisfied_from_cache_vmem.is_null() ? 1 : 0);
    const int num_vmems_a2 = a2._partial_vmems.length() + (a2._satisfied_from_cache_vmem.is_null() ? 1 : 0);
    _partial_vmems.reserve(num_vmems_a1 + num_vmems_a2);

    // Transfer the claimed capacity
    transfer_claimed_capacity(a1);
    transfer_claimed_capacity(a2);
  }

  void transfer_claimed_capacity(const ZMemoryAllocation& from) {
    assert(from._committed_capacity == 0, "Unexpected value %zu", from._committed_capacity);
    assert(!from._commit_failed, "Unexpected value");

    // Transfer increased capacity
    _increased_capacity += from._increased_capacity;

    // Transfer satisfying vmem or partial mappings
    const ZVirtualMemory vmem = from._satisfied_from_cache_vmem;
    if (!vmem.is_null()) {
      assert(_partial_vmems.is_empty(), "Must either have result or partial vmems");
      _partial_vmems.push(vmem);
      _num_harvested += 1;
      _harvested += vmem.size();
    } else {
      _partial_vmems.appendAll(&from._partial_vmems);
      _num_harvested += from._num_harvested;
      _harvested += from._harvested;
    }
  }

public:
  explicit ZMemoryAllocation(size_t size)
    : _size(size),
      _partition(nullptr),
      _satisfied_from_cache_vmem(),
      _partial_vmems(0),
      _num_harvested(0),
      _harvested(0),
      _increased_capacity(0),
      _committed_capacity(0),
      _commit_failed(false) {}

  void reset_for_retry() {
    assert(_satisfied_from_cache_vmem.is_null(), "Incompatible with reset");

    _partition = nullptr;
    _partial_vmems.clear();
    _num_harvested = 0;
    _harvested = 0;
    _increased_capacity = 0;
    _committed_capacity = 0;
    _commit_failed = false;
  }

  size_t size() const {
    return _size;
  }

  ZPartition& partition() const {
    assert(_partition != nullptr, "Should have been initialized");
    return *_partition;
  }

  void set_partition(ZPartition* partition) {
    assert(_partition == nullptr, "Should be initialized only once");
    _partition = partition;
  }

  ZVirtualMemory satisfied_from_cache_vmem() const {
    return _satisfied_from_cache_vmem;
  }

  void set_satisfied_from_cache_vmem_fast_medium(ZVirtualMemory vmem) {
    precond(_satisfied_from_cache_vmem.is_null());
    precond(_partial_vmems.is_empty());
    precond(ZPageSizeMediumEnabled);
    precond(vmem.size() >= ZPageSizeMediumMin);
    precond(vmem.size() <= ZPageSizeMediumMax);
    precond(is_power_of_2(vmem.size()));

    _satisfied_from_cache_vmem = vmem;
  }

  void set_satisfied_from_cache_vmem(ZVirtualMemory vmem) {
    precond(_satisfied_from_cache_vmem.is_null());
    precond(vmem.size() == size());
    precond(_partial_vmems.is_empty());

    _satisfied_from_cache_vmem = vmem;
  }

  ZArray<ZVirtualMemory>* partial_vmems() {
    return &_partial_vmems;
  }

  const ZArray<ZVirtualMemory>* partial_vmems() const {
    return &_partial_vmems;
  }

  int num_harvested() const {
    return _num_harvested;
  }

  size_t harvested() const {
    return _harvested;
  }

  void set_harvested(int num_harvested, size_t harvested) {
    _num_harvested = num_harvested;
    _harvested = harvested;
  }

  size_t increased_capacity() const {
    return _increased_capacity;
  }

  void set_increased_capacity(size_t increased_capacity) {
    _increased_capacity = increased_capacity;
  }

  size_t committed_capacity() const {
    return _committed_capacity;
  }

  void set_committed_capacity(size_t committed_capacity) {
    assert(_committed_capacity == 0, "Should only commit once");
    _committed_capacity = committed_capacity;
    _commit_failed = committed_capacity != _increased_capacity;
  }

  bool commit_failed() const {
    return _commit_failed;
  }

  static void destroy(ZMemoryAllocation* allocation) {
    delete allocation;
  }

  static void merge(const ZMemoryAllocation& allocation, ZMemoryAllocation** merge_location) {
    ZMemoryAllocation* const other_allocation = *merge_location;
    if (other_allocation == nullptr) {
      // First allocation, allocate new partition
      *merge_location = new ZMemoryAllocation(allocation);
    } else {
      // Merge with other allocation
      *merge_location = new ZMemoryAllocation(allocation, *other_allocation);

      // Delete old allocation
      delete other_allocation;
    }
  }
};

class ZSinglePartitionAllocation {
private:
  ZMemoryAllocation _allocation;

public:
  ZSinglePartitionAllocation(size_t size)
    : _allocation(size) {}

  size_t size() const {
    return _allocation.size();
  }

  ZMemoryAllocation* allocation() {
    return &_allocation;
  }

  const ZMemoryAllocation* allocation() const {
    return &_allocation;
  }

  void reset_for_retry() {
    _allocation.reset_for_retry();
  }
};

class ZMultiPartitionAllocation : public StackObj {
private:
  const size_t               _size;
  ZArray<ZMemoryAllocation*> _allocations;

public:
  ZMultiPartitionAllocation(size_t size)
    : _size(size),
      _allocations(0) {}

  ~ZMultiPartitionAllocation() {
    for (ZMemoryAllocation* allocation : _allocations) {
      ZMemoryAllocation::destroy(allocation);
    }
  }

  void initialize() {
    precond(_allocations.is_empty());

    // The multi-partition allocation creates at most one allocation per partition.
    const int length = (int)ZNUMA::count();

    _allocations.reserve(length);
  }

  void reset_for_retry() {
    for (ZMemoryAllocation* allocation : _allocations) {
      ZMemoryAllocation::destroy(allocation);
    }
    _allocations.clear();
  }

  size_t size() const {
    return _size;
  }

  ZArray<ZMemoryAllocation*>* allocations() {
    return &_allocations;
  }

  const ZArray<ZMemoryAllocation*>* allocations() const {
    return &_allocations;
  }

  void register_allocation(const ZMemoryAllocation& allocation) {
    ZMemoryAllocation** const slot = allocation_slot(allocation.partition().numa_id());

    ZMemoryAllocation::merge(allocation, slot);
  }

  ZMemoryAllocation** allocation_slot(uint32_t numa_id) {
    // Try to find an existing allocation for numa_id
    for (int i = 0; i < _allocations.length(); ++i) {
      ZMemoryAllocation** const slot_addr = _allocations.adr_at(i);
      ZMemoryAllocation* const allocation = *slot_addr;
      if (allocation->partition().numa_id() == numa_id) {
        // Found an existing slot
        return slot_addr;
      }
    }

    // Push an empty slot for the numa_id
    _allocations.push(nullptr);

    // Return the address of the slot
    return &_allocations.last();
  }

  int sum_num_harvested_vmems() const {
    int total = 0;

    for (const ZMemoryAllocation* allocation : _allocations) {
      total += allocation->num_harvested();
    }

    return total;
  }

  size_t sum_harvested() const {
    size_t total = 0;

    for (const ZMemoryAllocation* allocation : _allocations) {
      total += allocation->harvested();
    }

    return total;
  }

  size_t sum_committed_increased_capacity() const {
    size_t total = 0;

    for (const ZMemoryAllocation* allocation : _allocations) {
      total += allocation->committed_capacity();
    }

    return total;
  }
};

struct ZPageAllocationStats {
  int    _num_harvested_vmems;
  size_t _total_harvested;
  size_t _total_committed_capacity;

  ZPageAllocationStats(int num_harvested_vmems, size_t total_harvested, size_t total_committed_capacity)
    : _num_harvested_vmems(num_harvested_vmems),
      _total_harvested(total_harvested),
      _total_committed_capacity(total_committed_capacity) {}
};

class ZPageAllocation : public StackObj {
  friend class ZList<ZPageAllocation>;

private:
  const ZPageType            _type;
  const size_t               _requested_size;
  const ZAllocationFlags     _flags;
  const ZPageAge             _age;
  const Ticks                _start_timestamp;
  const uint32_t             _young_seqnum;
  const uint32_t             _old_seqnum;
  const uint32_t             _preferred_partition;
  bool                       _is_multi_partition;
  ZSinglePartitionAllocation _single_partition_allocation;
  ZMultiPartitionAllocation  _multi_partition_allocation;
  ZListNode<ZPageAllocation> _node;
  ZFuture<bool>              _stall_result;

public:
  ZPageAllocation(ZPageType type, size_t size, ZAllocationFlags flags, ZPageAge age, uint32_t preferred_partition)
    : _type(type),
      _requested_size(size),
      _flags(flags),
      _age(age),
      _start_timestamp(Ticks::now()),
      _young_seqnum(ZGeneration::young()->seqnum()),
      _old_seqnum(ZGeneration::old()->seqnum()),
      _preferred_partition(preferred_partition),
      _is_multi_partition(false),
      _single_partition_allocation(size),
      _multi_partition_allocation(size),
      _node(),
      _stall_result() {
    assert(_preferred_partition < ZNUMA::count(), "Preferred partition out-of-bounds (0 <= %d < %d)", _preferred_partition, ZNUMA::count());
  }

  void reset_for_retry() {
    _is_multi_partition = false;
    _single_partition_allocation.reset_for_retry();
    _multi_partition_allocation.reset_for_retry();
  }

  ZPageType type() const {
    return _type;
  }

  size_t size() const {
    if (_flags.fast_medium()) {
      // A fast medium allocation may have allocated less than the _size field
      const ZVirtualMemory vmem = _single_partition_allocation.allocation()->satisfied_from_cache_vmem();
      if (!vmem.is_null()) {
        // The allocation has been satisfied, return the satisfied size.
        return vmem.size();
      }
    }

    return _requested_size;
  }

  ZAllocationFlags flags() const {
    return _flags;
  }

  ZPageAge age() const {
    return _age;
  }

  uint32_t young_seqnum() const {
    return _young_seqnum;
  }

  uint32_t old_seqnum() const {
    return _old_seqnum;
  }

  uint32_t preferred_partition() const {
    return _preferred_partition;
  }

  bool is_multi_partition() const {
    return _is_multi_partition;
  }

  void initiate_multi_partition_allocation() {
    assert(!_is_multi_partition, "Reinitialization?");
    _is_multi_partition = true;
    _multi_partition_allocation.initialize();
  }

  ZMultiPartitionAllocation* multi_partition_allocation() {
    assert(_is_multi_partition, "multi-partition allocation must be initiated");

    return &_multi_partition_allocation;
  }

  const ZMultiPartitionAllocation* multi_partition_allocation() const {
    assert(_is_multi_partition, "multi-partition allocation must be initiated");

    return &_multi_partition_allocation;
  }

  ZSinglePartitionAllocation* single_partition_allocation() {
    assert(!_is_multi_partition, "multi-partition allocation must not have been initiated");

    return &_single_partition_allocation;
  }

  const ZSinglePartitionAllocation* single_partition_allocation() const {
    assert(!_is_multi_partition, "multi-partition allocation must not have been initiated");

    return &_single_partition_allocation;
  }

  ZVirtualMemory satisfied_from_cache_vmem() const {
    precond(!_is_multi_partition);

    const ZMemoryAllocation* const allocation = _single_partition_allocation.allocation();

    return allocation->satisfied_from_cache_vmem();
  }

  bool wait() {
    return _stall_result.get();
  }

  void satisfy(bool result) {
    _stall_result.set(result);
  }

  bool gc_relocation() const {
    return _flags.gc_relocation();
  }

  ZPageAllocationStats stats() const {
    if (_is_multi_partition) {
      return ZPageAllocationStats(
          _multi_partition_allocation.sum_num_harvested_vmems(),
          _multi_partition_allocation.sum_harvested(),
          _multi_partition_allocation.sum_committed_increased_capacity());
    } else {
      return ZPageAllocationStats(
          _single_partition_allocation.allocation()->num_harvested(),
          _single_partition_allocation.allocation()->harvested(),
          _single_partition_allocation.allocation()->committed_capacity());
    }
  }

  void send_event(bool successful) {
    if (!EventZPageAllocation::is_enabled()) {
      // Event not enabled, exit early
      return;
    }

    Ticks end_timestamp = Ticks::now();
    const ZPageAllocationStats st = stats();

    EventZPageAllocation::commit(_start_timestamp,
                                 end_timestamp,
                                 (u8)_type,
                                 size(),
                                 st._total_harvested,
                                 st._total_committed_capacity,
                                 (unsigned)st._num_harvested_vmems,
                                 _is_multi_partition,
                                 successful,
                                 _flags.non_blocking());
  }
};

const ZVirtualMemoryManager& ZPartition::virtual_memory_manager() const {
  return _page_allocator->_virtual;
}

ZVirtualMemoryManager& ZPartition::virtual_memory_manager() {
  return _page_allocator->_virtual;
}

const ZPhysicalMemoryManager& ZPartition::physical_memory_manager() const {
  return _page_allocator->_physical;
}

ZPhysicalMemoryManager& ZPartition::physical_memory_manager() {
  return _page_allocator->_physical;
}

ZLock* ZPartition::lock() const {
  return &_page_allocator->_lock;
}

#ifdef ASSERT

void ZPartition::verify_virtual_memory_multi_partition_association(const ZVirtualMemory& vmem) const {
  const ZVirtualMemoryManager& manager = virtual_memory_manager();

  assert(manager.is_in_multi_partition(vmem),
         "Virtual memory must be associated with the extra space "
         "actual: %u", virtual_memory_manager().lookup_partition_id(vmem));
}

void ZPartition::verify_virtual_memory_association(const ZVirtualMemory& vmem, bool check_multi_partition) const {
  const ZVirtualMemoryManager& manager = virtual_memory_manager();

  if (check_multi_partition && manager.is_in_multi_partition(vmem)) {
    // We allow claim/free/commit physical operation in multi-partition allocations
    // to use virtual memory associated with the extra space.
    return;
  }

  const uint32_t vmem_numa_id = virtual_memory_manager().lookup_partition_id(vmem);
  assert(_numa_id == vmem_numa_id,
         "Virtual memory must be associated with the current partition "
         "expected: %u, actual: %u", _numa_id, vmem_numa_id);
}

void ZPartition::verify_virtual_memory_association(const ZArray<ZVirtualMemory>* vmems) const {
  for (const ZVirtualMemory& vmem : *vmems) {
    verify_virtual_memory_association(vmem);
  }
}

void ZPartition::verify_memory_allocation_association(const ZMemoryAllocation* allocation) const {
  assert(this == &allocation->partition(),
         "Memory allocation must be associated with the current partition "
         "expected: %u, actual: %u", _numa_id, allocation->partition().numa_id());
}

#endif // ASSERT

ZPartition::ZPartition(uint32_t numa_id,
                       ZPageAllocator* page_allocator,
                       size_t min_capacity,
                       size_t initial_capacity,
                       size_t static_max_capacity)
  : _page_allocator(page_allocator),
    _cache(),
    _uncommitter(numa_id, this),
    _mem_worker(numa_id, this),
    _min_capacity(ZNUMA::calculate_share(numa_id, min_capacity)),
    _static_max_capacity(ZNUMA::calculate_share(numa_id, static_max_capacity)),
    _committed(0),
    _observed_max_committed(_static_max_capacity),
    _capacity(0),
    _claimed(0),
    _used(0),
    _numa_id(numa_id) {}

size_t ZPartition::dynamic_max_capacity() const {
  return ZNUMA::calculate_share(_numa_id, _page_allocator->dynamic_max_capacity());
}

size_t ZPartition::current_max_capacity() const {
  return ZNUMA::calculate_share(_numa_id, _page_allocator->current_max_capacity());
}

size_t ZPartition::static_max_capacity() const {
  return _static_max_capacity;
}

size_t ZPartition::capacity() const {
  return Atomic::load(&_capacity);
}

void ZPartition::increase_committed(size_t increment, bool commit_failed) {
  _committed += increment;

  if (commit_failed) {
    // Commit failed, reset max
    _observed_max_committed = _committed;
  } else if (_committed > _observed_max_committed) {
    // Commit successful update max
    _observed_max_committed = _committed;
  }
}

void ZPartition::decrease_committed(size_t decrement) {
  _committed -= decrement;
}

const ZUncommitter& ZPartition::uncommitter() const {
  return _uncommitter;
}

ZUncommitter& ZPartition::uncommitter() {
  return _uncommitter;
}

const ZMemoryWorker& ZPartition::memory_worker() const {
  return _mem_worker;
}

ZMemoryWorker& ZPartition::memory_worker() {
  return _mem_worker;
}

uint32_t ZPartition::numa_id() const {
  return _numa_id;
}

size_t ZPartition::available(size_t limit) const {
  assert(_capacity == _used + _claimed + _cache.size(), "Should be consistent"
          " _capacity: %zx _used: %zx _claimed: %zx _cache.size(): %zx",
          _capacity, _used, _claimed, _cache.size());
  assert(limit <= _static_max_capacity, "Invalid limit for partition");
  const size_t unavailable = _used + _claimed;

  if (limit < unavailable) {
    // The current max capacity may be below what is handed out.
    return 0;
  }

  return limit - unavailable;
}

size_t ZPartition::available(ZPageAllocationAttempt attempt, size_t limit) const {
  assert(_capacity == _used + _claimed + _cache.size(), "Should be consistent"
          " _capacity: %zx _used: %zx _claimed: %zx _cache.size(): %zx",
          _capacity, _used, _claimed, _cache.size());

  if (attempt == ZPageAllocationAttempt::initial) {
    return available(limit);
  }

  if (attempt == ZPageAllocationAttempt::retry) {
    return available_from_cache(limit);
  }

  if (attempt == ZPageAllocationAttempt::stall) {
    return available_from_observed_max_committed(limit);
  }

  ShouldNotReachHere();
}

size_t ZPartition::available_from_increase_capacity(size_t limit) const {
  // Available includes the cache and what we can increase capacity by.
  const size_t available = ZPartition::available(limit);

  const size_t cached = _cache.size();

  if (available < cached) {
    // The current allowed available may be bellow what is in the cache
    return 0;
  }

  return available - cached;
}

size_t ZPartition::available_from_cache(size_t limit) const {
  // Available includes the cache and what we can increase capacity by.
  const size_t available = ZPartition::available(limit);
  const size_t cached = _cache.size();

  // The current allowed available may be bellow what is in the cache
  return MIN2(available, cached);
}

size_t ZPartition::available_from_observed_max_committed(size_t limit) const {
  const size_t unavailable = _used + _claimed;
  const size_t available_observed_max_committed = _observed_max_committed > unavailable
      ? _observed_max_committed - unavailable : 0;
  const size_t available = ZPartition::available(limit);

  return MIN2(available, available_observed_max_committed);
}

size_t ZPartition::increase_capacity(size_t size, ZPageAllocationAttempt attempt, size_t limit) {
  if (attempt == ZPageAllocationAttempt::initial) {
    return increase_capacity(size, limit);
  }

  if (attempt == ZPageAllocationAttempt::retry) {
    return 0;
  }

  if (attempt == ZPageAllocationAttempt::stall) {
    assert(available_from_observed_max_committed(limit) >= available_from_cache(limit), "must be");
    const size_t available = available_from_observed_max_committed(limit) - available_from_cache(limit);
    return increase_capacity(MIN2(size, available), limit);
  }

  ShouldNotReachHere();
}

size_t ZPartition::increase_capacity(size_t size, size_t limit) {
  if (_capacity >= limit) {
    return 0;
  }

  const size_t available = available_from_increase_capacity(limit);
  const size_t increased = MIN2(size, available);

  if (increased > 0) {
    // Update atomically since we have concurrent readers
    Atomic::add(&_capacity, increased);

    _uncommitter.cancel_uncommit_cycle();
  }

  return increased;
}

void ZPartition::decrease_capacity(size_t size) {
  // Update capacity atomically since we have concurrent readers
  Atomic::sub(&_capacity, size);
}

void ZPartition::increase_used(size_t size) {
  // The partition usage tracking is only read and updated under the page
  // allocator lock. Usage statistics for generations and GC cycles are
  // collected on the ZPageAllocator level.
  _used += size;
}

void ZPartition::decrease_used(size_t size) {
  // The partition usage tracking is only read and updated under the page
  // allocator lock. Usage statistics for generations and GC cycles are
  // collected on the ZPageAllocator level.
  _used -= size;
}

static void pretouch_memory(zoffset start, size_t size) {
  // At this point we know that we have a valid zoffset / zaddress.
  const zaddress zaddr = ZOffset::address(start);
  const uintptr_t addr = untype(zaddr);
  const size_t page_size = ZLargePages::is_explicit() ? ZGranuleSize : os::vm_page_size();
  os::pretouch_memory((void*)addr, (void*)(addr + size), page_size);
}

void ZPartition::heat_memory(const ZVirtualMemory& vmem) const {
  verify_virtual_memory_association(vmem, true /* check_multi_partition */);

  const ZPhysicalMemoryManager& manager = physical_memory_manager();

  pretouch_memory(vmem.start(), vmem.size());
  if (ZLargePages::is_collapse()) {
    manager.collapse(vmem);
  }
}

void ZPartition::free_memory(const ZVirtualMemory& vmem) {
  const size_t size = vmem.size();

  // Cache the vmem
  _cache.insert(vmem);

  // Update accounting
  decrease_used(size);
}

void ZPartition::claim_from_cache_or_increase_capacity(ZMemoryAllocation* allocation, ZPageAllocationAttempt attempt, size_t limit) {
  const size_t size = allocation->size();
  ZArray<ZVirtualMemory>* const out = allocation->partial_vmems();

  // We are guaranteed to succeed the claiming of capacity here
  assert(available(attempt, limit), "Must be");

  // Associate the allocation with this partition.
  allocation->set_partition(this);

  // Try to allocate one contiguous vmem
  ZVirtualMemory vmem = _cache.remove_contiguous(size);
  if (!vmem.is_null()) {
    // Found a satisfying vmem in the cache
    allocation->set_satisfied_from_cache_vmem(vmem);

    // Done
    return;
  }

  // Try increase capacity
  const size_t increased_capacity = increase_capacity(size, attempt, limit);

  allocation->set_increased_capacity(increased_capacity);

  if (increased_capacity == size) {
    // Capacity increase covered the entire request, done.
    return;
  }

  // Could not increase capacity enough to satisfy the allocation completely.
  // Try removing multiple vmems from the mapped cache.
  const size_t remaining = size - increased_capacity;
  const size_t harvested = _cache.remove_discontiguous(remaining, out);
  const int num_harvested = out->length();

  allocation->set_harvested(num_harvested, harvested);

  assert(harvested + increased_capacity == size,
         "Mismatch harvested: %zu increased_capacity: %zu size: %zu",
         harvested / M, increased_capacity / M, size / M);

  return;
}

bool ZPartition::claim_capacity(ZMemoryAllocation* allocation, ZPageAllocationAttempt attempt, size_t limit) {
  const size_t size = allocation->size();

  if (available(attempt, limit) < size) {
    // Out of memory
    return false;
  }

  claim_from_cache_or_increase_capacity(allocation, attempt, limit);

  // Updated used statistics
  increase_used(size);

  // Success
  return true;
}

bool ZPartition::claim_capacity_fast_medium(ZMemoryAllocation* allocation, size_t limit) {
  precond(ZPageSizeMediumEnabled);

  // Try to allocate a medium page sized contiguous vmem
  const size_t available_from_cache_limit = available_from_cache(limit);
  const size_t power_of_2_limit = available_from_cache_limit == 0
      ? 0
      : round_down_power_of_2(available_from_cache_limit);
  const size_t min_size = ZPageSizeMediumMin;

  if (power_of_2_limit < min_size) {
    // No medium allocation size available within the limit.
    return false;
  }

  const size_t max_size = MIN2(ZStressFastMediumPageAllocation ? min_size : ZPageSizeMediumMax, power_of_2_limit);
  ZVirtualMemory vmem = _cache.remove_contiguous_power_of_2(min_size, max_size);

  if (vmem.is_null()) {
    // Failed to find a contiguous vmem
    return false;
  }

  // Found a satisfying vmem in the cache
  allocation->set_satisfied_from_cache_vmem_fast_medium(vmem);

  // Associate the allocation with this partition.
  allocation->set_partition(this);

  // Updated used statistics
  increase_used(vmem.size());

  // Success
  return true;
}

size_t ZPartition::commit(size_t size, size_t limit) {
  size_t commit_size;
  {
    ZLocker<ZLock> locker(lock());

    commit_size = increase_capacity(size, limit);

    if (commit_size == 0) {
      return 0;
    }

    // Account for both the node and the page allocator
    increase_used(commit_size);
    _page_allocator->increase_used(commit_size);
  }

  ZArray<ZVirtualMemory> vmems;

  const size_t claimed_virtual = claim_virtual(commit_size, &vmems);

  assert(claimed_virtual == commit_size, "must succeed");

  size_t total_committed = 0;
  bool commit_failed = false;
  ZArray<ZVirtualMemory> to_free(vmems.length());
  for (ZVirtualMemory vmem : vmems) {
    if (commit_failed) {
      // If a commit has failed free any remaining vmems
      free_virtual(vmem);
      continue;
    }

    // Claim physical
    claim_physical(vmem);

    // Commit memory
    const size_t committed = commit_physical(vmem);

    // Keep track of total committed
    total_committed += committed;

    if (committed != vmem.size()) {
      commit_failed = true;
      const ZVirtualMemory not_committed_vmem = vmem.shrink_from_back(vmem.size() - committed);
      free_physical(not_committed_vmem);
      free_virtual(not_committed_vmem);
    }

    if (vmem.size() != 0) {
      // Map memory
      map_virtual(vmem, false /* heat */);

      // Heat memory
      heat_memory(vmem);

      to_free.push(vmem);
    }
  }

  if (commit_failed) {
    // A commit has failed

    ZLocker<ZLock> locker(lock());
    const size_t not_committed = commit_size - total_committed;
    decrease_capacity(not_committed);
    _page_allocator->truncate_heuristic_max_after_commit_failure();

    // Account for both the node and the page allocator
    decrease_used(not_committed);
    _page_allocator->decrease_used(not_committed);
  }

  // Free the memory
  _page_allocator->free_memory(&to_free);

  return total_committed;
}

void ZPartition::sort_segments_physical(const ZVirtualMemory& vmem) {
  verify_virtual_memory_association(vmem, true /* check_multi_partition */);

  ZPhysicalMemoryManager& manager = physical_memory_manager();

  // Sort physical segments
  manager.sort_segments_physical(vmem);
}

void ZPartition::claim_physical(const ZVirtualMemory& vmem) {
  verify_virtual_memory_association(vmem, true /* check_multi_partition */);

  ZPhysicalMemoryManager& manager = physical_memory_manager();

  // Alloc physical memory
  manager.alloc(vmem, _numa_id);
}

void ZPartition::free_physical(const ZVirtualMemory& vmem) {
  verify_virtual_memory_association(vmem, true /* check_multi_partition */);

  ZPhysicalMemoryManager& manager = physical_memory_manager();

  // Free physical memory
  manager.free(vmem, _numa_id);
}

size_t ZPartition::commit_physical(const ZVirtualMemory& vmem) {
  verify_virtual_memory_association(vmem, true /* check_multi_partition */);

  ZPhysicalMemoryManager& manager = physical_memory_manager();

  // Commit physical memory
  const size_t committed =  manager.commit(vmem, _numa_id);

  // Keep track of the committed memory
  {
    ZLocker<ZLock> locker(lock());
    const bool commit_failed = committed != vmem.size();
    increase_committed(committed, commit_failed);
  }

  return committed;
}

size_t ZPartition::uncommit_physical(const ZVirtualMemory& vmem) {
  assert(ZUncommit, "should not uncommit when uncommit is disabled");
  verify_virtual_memory_association(vmem);

  ZPhysicalMemoryManager& manager = physical_memory_manager();

  // Uncommit physical memory
  const size_t uncommitted = manager.uncommit(vmem);

  // Keep track of the committed memory
  {
    ZLocker<ZLock> locker(lock());
    decrease_committed(uncommitted);
  }

  return uncommitted;
}

void ZPartition::map_virtual(const ZVirtualMemory& vmem, bool heat_memory) {
  verify_virtual_memory_association(vmem);

  ZPhysicalMemoryManager& manager = physical_memory_manager();

  // Map virtual memory to physical memory
  manager.map(vmem, _numa_id);

  if (heat_memory && ZAdaptiveHeap::can_adapt()) {
    // Register a heating request for this mapping
    _mem_worker.register_heating_request(vmem);
  }
}

void ZPartition::unmap_virtual(const ZVirtualMemory& vmem) {
  verify_virtual_memory_association(vmem);

  if (ZAdaptiveHeap::can_adapt()) {
    // Remove any heating request before unmapping
    _mem_worker.remove_heating_request(vmem);
  }

  ZPhysicalMemoryManager& manager = physical_memory_manager();

  // Unmap virtual memory from physical memory
  manager.unmap(vmem);
}

void ZPartition::map_virtual_from_multi_partition(const ZVirtualMemory& vmem) {
  verify_virtual_memory_multi_partition_association(vmem);

  ZPhysicalMemoryManager& manager = physical_memory_manager();

  // Sort physical segments
  manager.sort_segments_physical(vmem);

  // Map virtual memory to physical memory
  manager.map(vmem, _numa_id);

  // Register a heating request for this mapping
  if (ZAdaptiveHeap::can_adapt()) {
    _mem_worker.register_heating_request(vmem);
  }
}

void ZPartition::unmap_virtual_from_multi_partition(const ZVirtualMemory& vmem) {
  verify_virtual_memory_multi_partition_association(vmem);

  // Remove any heating request before unmapping
  if (ZAdaptiveHeap::can_adapt()) {
    _mem_worker.remove_heating_request(vmem);
  }

  ZPhysicalMemoryManager& manager = physical_memory_manager();

  // Unmap virtual memory from physical memory
  manager.unmap(vmem);
}

ZVirtualMemory ZPartition::claim_virtual(size_t size) {
  ZVirtualMemoryManager& manager = virtual_memory_manager();

  return manager.remove_from_low(size, _numa_id);
}

size_t ZPartition::claim_virtual(size_t size, ZArray<ZVirtualMemory>* vmems_out) {
  ZVirtualMemoryManager& manager = virtual_memory_manager();

  return manager.remove_from_low_many_at_most(size, _numa_id, vmems_out);
}

void ZPartition::free_virtual(const ZVirtualMemory& vmem) {
  verify_virtual_memory_association(vmem);

  ZVirtualMemoryManager& manager = virtual_memory_manager();

  // Free virtual memory
  manager.insert(vmem, _numa_id);
}

void ZPartition::free_and_claim_virtual_from_low_many(const ZVirtualMemory& vmem, ZArray<ZVirtualMemory>* vmems_out) {
  verify_virtual_memory_association(vmem);

  ZVirtualMemoryManager& manager = virtual_memory_manager();

  // Shuffle virtual memory
  manager.insert_and_remove_from_low_many(vmem, _numa_id, vmems_out);
}

ZVirtualMemory ZPartition::free_and_claim_virtual_from_low_exact_or_many(size_t size, ZArray<ZVirtualMemory>* vmems_in_out) {
  verify_virtual_memory_association(vmems_in_out);

  ZVirtualMemoryManager& manager = virtual_memory_manager();

  // Shuffle virtual memory
  return manager.insert_and_remove_from_low_exact_or_many(size, _numa_id, vmems_in_out);
}

class ZPreTouchTask : public ZTask {
private:
  volatile uintptr_t _current;
  const uintptr_t    _end;

public:
  ZPreTouchTask(zoffset start, zoffset_end end)
    : ZTask("ZPreTouchTask"),
      _current(untype(start)),
      _end(untype(end)) {}

  virtual void work() {
    const size_t size = ZGranuleSize;

    for (;;) {
      // Claim an offset for this thread
      const uintptr_t claimed = Atomic::fetch_then_add(&_current, size);
      if (claimed >= _end) {
        // Done
        break;
      }

      // At this point we know that we have a valid zoffset / zaddress.
      const zoffset offset = to_zoffset(claimed);

      // Pre-touch the granule
      pretouch_memory(offset, size);
    }
  }
};

bool ZPartition::prime(ZWorkers* workers, size_t size, size_t limit) {
  if (size == 0) {
    return true;
  }

  ZArray<ZVirtualMemory> vmems;

  // Claim virtual memory
  const size_t claimed_size = claim_virtual(size, &vmems);

  // The partition must have size available in virtual memory when priming.
  assert(claimed_size == size, "must succeed %zx == %zx", claimed_size, size);

  // Increase capacity
  increase_capacity(claimed_size, limit);

  for (ZVirtualMemory vmem : vmems) {
    // Claim the backing physical memory
    claim_physical(vmem);

    // Commit the claimed physical memory
    const size_t committed = commit_physical(vmem);

    if (committed != vmem.size()) {
      // This is a failure state. We do not cleanup the maybe partially committed memory.
      return false;
    }

    map_virtual(vmem);

    check_numa_mismatch(vmem, _numa_id);

    if (AlwaysPreTouch) {
      // Pre-touch memory
      ZPreTouchTask task(vmem.start(), vmem.end());
      workers->run_all(&task);
    }

    // We don't have to take a lock here as no other threads will access the cache
    // until we're finished
    _cache.insert(vmem);
  }

  return true;
}

ZVirtualMemory ZPartition::prepare_harvested_and_claim_virtual(ZMemoryAllocation* allocation) {
  verify_memory_allocation_association(allocation);

  // Unmap virtual memory
  for (const ZVirtualMemory vmem : *allocation->partial_vmems()) {
    unmap_virtual(vmem);
  }

  const size_t harvested = allocation->harvested();
  const int granule_count = (int)(harvested >> ZGranuleSizeShift);
  ZPhysicalMemoryManager& manager = physical_memory_manager();

  // Stash segments
  ZArray<zbacking_index> stash(granule_count);
  manager.stash_segments(*allocation->partial_vmems(), &stash);

  // Shuffle virtual memory. We attempt to allocate enough memory to cover the
  // entire allocation size, not just for the harvested memory.
  const ZVirtualMemory result = free_and_claim_virtual_from_low_exact_or_many(allocation->size(), allocation->partial_vmems());

  // Restore segments
  if (!result.is_null()) {
    // Got exact match. Restore stashed physical segments for the harvested part.
    manager.restore_segments(result.first_part(harvested), stash);
  } else {
    // Got many partial vmems
    manager.restore_segments(*allocation->partial_vmems(), stash);
  }

  if (result.is_null()) {
    // Before returning harvested memory to the cache it must be mapped.
    for (const ZVirtualMemory vmem : *allocation->partial_vmems()) {
      map_virtual(vmem);
    }
  }

  return result;
}

void ZPartition::copy_physical_segments_to_partition(const ZVirtualMemory& at, const ZVirtualMemory& from) {
  verify_virtual_memory_association(at);
  verify_virtual_memory_association(from, true /* check_multi_partition */);

  ZPhysicalMemoryManager& manager = physical_memory_manager();

  // Copy segments
  manager.copy_physical_segments(at, from);
}

void ZPartition::copy_physical_segments_from_partition(const ZVirtualMemory& at, const ZVirtualMemory& to) {
  verify_virtual_memory_association(at);
  verify_virtual_memory_association(to, true /* check_multi_partition */);

  ZPhysicalMemoryManager& manager = physical_memory_manager();


  // Copy segments
  manager.copy_physical_segments(to, at);
}

void ZPartition::commit_increased_capacity(ZMemoryAllocation* allocation, const ZVirtualMemory& vmem) {
  assert(allocation->increased_capacity() > 0, "Nothing to commit");

  const size_t curr_max = _page_allocator->current_max_capacity();
  const size_t capacity = _page_allocator->capacity();

  if (capacity > curr_max) {
    // Not allowed to commit beyond current max capacity; bail
    allocation->set_committed_capacity(0);
    return;
  }

  const size_t already_committed = allocation->harvested();
  const ZVirtualMemory to_be_committed_vmem = vmem.last_part(already_committed);

  size_t commit_limit;

  if (ZAdaptiveHeap::explicit_max_capacity()) {
    // With a user supplied max heap size, everything may be committed.
    // If it's too much, it's a user error.
    commit_limit = _static_max_capacity;
  } else {
    // With an automatic max capacity, we have to be a bit careful not
    // to overprovision the machine
    commit_limit = align_down((curr_max - capacity) * 5 / 4, ZGranuleSize);
  }

  const size_t allowed_to_commit = MIN2(to_be_committed_vmem.size(), commit_limit);
  if (allowed_to_commit == 0) {
    // Out of memory; not allowed to commit more
    allocation->set_committed_capacity(0);
    return;
  }

  const ZVirtualMemory allowed_to_commit_vmem = to_be_committed_vmem.first_part(allowed_to_commit);

  // Try to commit the uncommitted physical memory
  const size_t committed = commit_physical(allowed_to_commit_vmem);

  // Keep track of the committed amount
  allocation->set_committed_capacity(committed);
}

void ZPartition::map_memory(ZMemoryAllocation* allocation, const ZVirtualMemory& vmem) {
  sort_segments_physical(vmem);
  map_virtual(vmem);

  check_numa_mismatch(vmem, allocation->partition().numa_id());
}

void ZPartition::free_memory_alloc_failed(ZMemoryAllocation* allocation) {
  verify_memory_allocation_association(allocation);

  // Only decrease the overall used and not the generation used,
  // since the allocation failed and generation used wasn't bumped.
  decrease_used(allocation->size());

  size_t freed = 0;

  // Free mapped memory
  for (const ZVirtualMemory vmem : *allocation->partial_vmems()) {
    freed += vmem.size();
    _cache.insert(vmem);
  }
  assert(allocation->harvested() + allocation->committed_capacity() == freed, "must have freed all"
         " %zu + %zu == %zu", allocation->harvested(), allocation->committed_capacity(), freed);

  // Adjust capacity to reflect the failed capacity increase
  const size_t remaining = allocation->size() - freed;
  if (remaining > 0) {
    decrease_capacity(remaining);
  }
}

void ZPartition::threads_do(ThreadClosure* tc) const {
  if (ZUncommit && !ZAdaptiveHeap::can_adapt()) {
    // ZUncommitter is not created
    tc->do_thread(const_cast<ZUncommitter*>(&_uncommitter));
  }
  if (ZAdaptiveHeap::can_adapt()) {
    tc->do_thread(const_cast<ZMemoryWorker*>(&_mem_worker));
  }
}

void ZPartition::print_on(outputStream* st) const {
  st->print("Partition %u ", _numa_id);
  st->fill_to(17);
  st->print_cr("used %zuM, capacity %zuM, max capacity %zuM",
               _used / M, _capacity / M, dynamic_max_capacity() / M);

  StreamIndentor si(st, 1);
  print_cache_on(st);
}

void ZPartition::print_cache_on(outputStream* st) const {
  _cache.print_on(st);
}

void ZPartition::print_cache_extended_on(outputStream* st) const {
  st->print_cr("Partition %u", _numa_id);

  StreamIndentor si(st, 1);
  _cache.print_extended_on(st);
}

class ZMultiPartitionTracker : CHeapObj<mtGC> {
private:
  struct Element {
    ZVirtualMemory _vmem;
    ZPartition*    _partition;
  };

  ZArray<Element> _map;

  ZMultiPartitionTracker(int capacity)
    : _map(capacity) {}

  const ZArray<Element>* map() const {
    return &_map;
  }

  ZArray<Element>* map() {
    return &_map;
  }

public:
  void prepare_memory_for_free(const ZVirtualMemory& vmem, ZArray<ZVirtualMemory>* vmems_out) const {
    // Remap memory back to original partition
    for (const Element partial_allocation : *map()) {
      ZVirtualMemory remaining_vmem = partial_allocation._vmem;
      ZPartition& partition = *partial_allocation._partition;

      const size_t size = remaining_vmem.size();

      // Allocate new virtual address ranges
      const int start_index = vmems_out->length();
      const size_t claimed_virtual = partition.claim_virtual(remaining_vmem.size(), vmems_out);

      // We are holding memory associated with this partition, and we do not
      // overcommit virtual memory claiming. So virtual memory must always
      // be available.
      assert(claimed_virtual == size, "must succeed");

      // Remap to the newly allocated virtual address ranges
      for (const ZVirtualMemory& to_vmem : vmems_out->slice_back(start_index)) {
        const ZVirtualMemory from_vmem = remaining_vmem.shrink_from_front(to_vmem.size());

        // Copy physical segments
        partition.copy_physical_segments_to_partition(to_vmem, from_vmem);

        // Unmap from_vmem
        partition.unmap_virtual_from_multi_partition(from_vmem);

        // Map to_vmem
        partition.map_virtual(to_vmem);
      }
      assert(remaining_vmem.size() == 0, "must have mapped all claimed virtual memory");
    }
  }

  static void destroy(const ZMultiPartitionTracker* tracker) {
    delete tracker;
  }

  static ZMultiPartitionTracker* create(const ZMultiPartitionAllocation* multi_partition_allocation, const ZVirtualMemory& vmem) {
    const ZArray<ZMemoryAllocation*>* const partial_allocations = multi_partition_allocation->allocations();

    ZMultiPartitionTracker* const tracker = new ZMultiPartitionTracker(partial_allocations->length());

    ZVirtualMemory remaining = vmem;

    // Each partial allocation is mapped to the virtual memory in order
    for (ZMemoryAllocation* partial_allocation : *partial_allocations) {
      // Track each separate vmem's partition
      const ZVirtualMemory partial_vmem = remaining.shrink_from_front(partial_allocation->size());
      ZPartition* const partition = &partial_allocation->partition();
      tracker->map()->push({partial_vmem, partition});
    }

    return tracker;
  }
};

ZPageAllocator::ZPageAllocator(size_t min_capacity,
                               size_t initial_capacity,
                               size_t soft_max_capacity,
                               size_t initial_max_capacity,
                               size_t static_max_capacity)
  : _lock(),
    _virtual(static_max_capacity),
    _physical(static_max_capacity),
    _min_capacity(min_capacity),
    _static_max_capacity(static_max_capacity),
    _heuristic_max_capacity(ZAdaptiveHeap::can_adapt() ? initial_capacity : static_max_capacity),
    _used(0),
    _used_generations{0,0},
    _collection_stats{{0, 0},{0, 0}},
    _partitions(ZValueIdTagType{}, this, min_capacity, initial_capacity, static_max_capacity),
    _stalled(),
    _safe_destroy(),
    _initialized(false) {

  if (!_virtual.is_initialized() || !_physical.is_initialized()) {
    return;
  }

  log_info_p(gc, init)("Min Capacity: %zuM", min_capacity / M);
  log_info_p(gc, init)("Initial Capacity: %zuM", initial_capacity / M);
  log_info_p(gc, init)("Max Capacity: %zuM", initial_max_capacity / M);
  log_info_p(gc, init)("Soft Max Capacity: %zuM", soft_max_capacity / M);
  if (ZPageSizeMediumEnabled) {
    if (ZPageSizeMediumMin == ZPageSizeMediumMax) {
      log_info_p(gc, init)("Page Size Medium: %zuM", ZPageSizeMediumMax / M);
    } else {
      log_info_p(gc, init)("Page Size Medium: Range [%zuM, %zuM]", ZPageSizeMediumMin / M, ZPageSizeMediumMax / M);
    }
  } else {
    log_info_p(gc, init)("Medium Page Size: N/A");
  }
  log_info_p(gc, init)("Pre-touch: %s", AlwaysPreTouch ? "Enabled" : "Disabled");
  ZAdaptiveHeap::print();

  // Warn if system limits could stop us from reaching max capacity
  size_t expected_capacity = ZAdaptiveHeap::explicit_max_capacity() ? initial_max_capacity
                                                                    : initial_capacity;
  _physical.warn_commit_limits(expected_capacity, initial_max_capacity);

  // Check if uncommit should and can be enabled
  _physical.try_enable_uncommit(min_capacity, static_max_capacity);

  // Successfully initialized
  _initialized = true;
}

bool ZPageAllocator::is_initialized() const {
  return _initialized;
}

bool ZPageAllocator::prime_cache(ZWorkers* workers, size_t size) {
  ZPartitionIterator iter = partition_iterator();
  for (ZPartition* partition; iter.next(&partition);) {
    const uint32_t numa_id = partition->numa_id();
    const size_t to_prime = ZNUMA::calculate_share(numa_id, size);
    const size_t partition_limit = ZNUMA::calculate_share(numa_id, _static_max_capacity);

    if (!partition->prime(workers, to_prime, partition_limit)) {
      return false;
    }
  }

  return true;
}

size_t ZPageAllocator::min_capacity() const {
  return _min_capacity;
}

size_t ZPageAllocator::static_max_capacity() const {
  return _static_max_capacity;
}

size_t ZPageAllocator::dynamic_max_capacity() const {
  if (ZAdaptiveHeap::explicit_max_capacity()) {
    return _static_max_capacity;
  }

  const size_t max = align_down(size_t(double(os::physical_memory()) * (1.0 - ZMemoryCriticalThreshold)), ZGranuleSize);
  return MAX2(max, _min_capacity);
}

size_t ZPageAllocator::current_max_capacity() const {
  if (ZAdaptiveHeap::explicit_max_capacity()) {
    // With an explicit max capacity we use that heap size
    return _static_max_capacity;
  }

  // Calculate current max capacity based on machine usage
  return ZAdaptiveHeap::current_max_capacity(capacity(), dynamic_max_capacity());
}

size_t ZPageAllocator::heuristic_max_capacity() const {
  // Note that SoftMaxHeapSize is a manageable flag
  const size_t soft_max_capacity = Atomic::load(&SoftMaxHeapSize);
  const size_t heuristic_max_capacity = Atomic::load(&_heuristic_max_capacity);

  size_t result;

  if (soft_max_capacity == 0) {
    // There is no human soft max heap, but the JVM has a clue
    result = heuristic_max_capacity;
  } else {
    // If there is both a user soft max and a JVM soft max, pick the smallest
    result = MIN2(soft_max_capacity, heuristic_max_capacity);
  }

  return align_down(result, ZGranuleSize);
}

void ZPageAllocator::adapt_heuristic_max_capacity(ZGenerationId generation) {
  const size_t soft_max_capacity = align_down(Atomic::load(&SoftMaxHeapSize), ZGranuleSize);
  const size_t heuristic_max = heuristic_max_capacity();
  const size_t min_capacity = _min_capacity;
  const size_t used = ZPageAllocator::used();
  const size_t capacity = MAX2(ZPageAllocator::capacity(), used);
  const size_t curr_max_capacity = MAX2(capacity, current_max_capacity());
  const size_t highest_soft_capacity = soft_max_capacity == 0 ? curr_max_capacity
                                                              : MIN2(soft_max_capacity, curr_max_capacity);
  const double alloc_rate = ZStatMutatorAllocRate::stats()._avg;

  ZHeapResizeMetrics metrics = {
    highest_soft_capacity,
    curr_max_capacity,
    heuristic_max,
    min_capacity,
    capacity,
    used,
    alloc_rate
  };

  const size_t selected_capacity = ZAdaptiveHeap::compute_heap_size(&metrics, generation);

  // Update heuristic max capacity
  Atomic::store(&_heuristic_max_capacity, selected_capacity);

  heap_resized(selected_capacity);
}

void ZPageAllocator::heap_resized(size_t selected_capacity) {
  precond(ZAdaptiveHeap::can_adapt());

  ZPerNUMAIterator<ZPartition> iter = partition_iterator();
  for (ZPartition* partition; iter.next(&partition);) {
    const uint32_t numa_id = partition->numa_id();

    // Update per partition heuristic max capacity
    const size_t selected_capacity_share = ZNUMA::calculate_share(numa_id, selected_capacity);

    // Update committer target capacity
    // TODO: Increase vs decrease?!
    ZMemoryWorker& mem_worker = partition->memory_worker();
    mem_worker.heap_resized(partition->capacity(), selected_capacity_share);
  }

  // Complain about misconfigurations
  _physical.warn_commit_limits(selected_capacity, dynamic_max_capacity());
}

void ZPageAllocator::heap_truncated(size_t selected_capacity) {
  if (!ZAdaptiveHeap::can_adapt()) {
    // ZMemoryWorker are only used with adaptive heap sizing.
    return;
  }

  ZPerNUMAIterator<ZPartition> iter = partition_iterator();
  for (ZPartition* partition; iter.next(&partition);) {
    const uint32_t numa_id = partition->numa_id();

    // Update per partition heuristic max capacity
    const size_t selected_capacity_share = ZNUMA::calculate_share(numa_id, selected_capacity);

    // Update committer target capacity
    ZMemoryWorker& mem_worker = partition->memory_worker();
    mem_worker.heap_truncated(selected_capacity_share);
  }

  // Complain about misconfigurations
  _physical.warn_commit_limits(selected_capacity, dynamic_max_capacity());
}

void ZPageAllocator::adjust_capacity(size_t used_soon) {
  const size_t total_memory = os::physical_memory();
    size_t used_memory = 0;
    if (!os::used_memory(used_memory)) {
      // TODO: Handle os::used_memory being unavailable.
    }
  const double uncommit_urgency = ZAdaptiveHeap::uncommit_urgency(used_memory, total_memory);

  ZPerNUMAIterator<ZPartition> iter = partition_iterator();
  for (ZPartition* partition; iter.next(&partition);) {

    if (uncommit_urgency > 0.0) {
      // Uncommit is urgent, or uncommit delay has changed
      ZMemoryWorker& mem_worker = partition->memory_worker();
      mem_worker.critical_shrink_target_capacity();
    } else {
      const uint32_t numa_id = partition->numa_id();
      const size_t used_soon_share = ZNUMA::calculate_share(numa_id, used_soon);
      ZMemoryWorker& mem_worker = partition->memory_worker();
      if (used_soon_share > mem_worker.target_capacity()) {
        mem_worker.grow_target_capacity(used_soon_share);
      }
    }
  }
}

size_t ZPageAllocator::capacity() const {
  size_t capacity = 0;

  ZPartitionConstIterator iter = partition_iterator();
  for (const ZPartition* partition; iter.next(&partition);) {
    capacity += Atomic::load(&partition->_capacity);
  }

  return capacity;
}

size_t ZPageAllocator::used() const {
  return Atomic::load(&_used);
}

size_t ZPageAllocator::used_generation(ZGenerationId id) const {
  return Atomic::load(&_used_generations[(int)id]);
}

size_t ZPageAllocator::unused() const {
  const ssize_t used = (ssize_t)ZPageAllocator::used();
  ssize_t capacity = 0;
  ssize_t claimed = 0;

  ZPartitionConstIterator iter = partition_iterator();
  for (const ZPartition* partition; iter.next(&partition);) {
    capacity += (ssize_t)Atomic::load(&partition->_capacity);
    claimed += (ssize_t)Atomic::load(&partition->_claimed);
  }

  const ssize_t unused = capacity - used - claimed;
  return unused > 0 ? (size_t)unused : 0;
}

void ZPageAllocator::update_collection_stats(ZGenerationId id) {
  assert(SafepointSynchronize::is_at_safepoint(), "Should be at safepoint");

#ifdef ASSERT
  size_t total_used = 0;

  ZPartitionIterator iter(&_partitions);
  for (ZPartition* partition; iter.next(&partition);) {
    total_used += partition->_used;
  }

  assert(total_used == _used, "Must be consistent %zu == %zu", total_used, _used);
#endif

  _collection_stats[(int)id]._used_high = _used;
  _collection_stats[(int)id]._used_low = _used;
}

ZPageAllocatorStats ZPageAllocator::stats_inner(ZGeneration* generation) const {
  return ZPageAllocatorStats(_min_capacity,
                             heuristic_max_capacity(),
                             capacity(),
                             _used,
                             _collection_stats[(int)generation->id()]._used_high,
                             _collection_stats[(int)generation->id()]._used_low,
                             used_generation(generation->id()),
                             generation->freed(),
                             generation->promoted(),
                             generation->compacted(),
                             _stalled.size());
}

ZPageAllocatorStats ZPageAllocator::stats(ZGeneration* generation) const {
  ZLocker<ZLock> locker(&_lock);
  return stats_inner(generation);
}

ZPageAllocatorStats ZPageAllocator::update_and_stats(ZGeneration* generation) {
  ZLocker<ZLock> locker(&_lock);

  update_collection_stats(generation->id());
  return stats_inner(generation);
}

void ZPageAllocator::increase_used_generation(ZGenerationId id, size_t size) {
  // Update atomically since we have concurrent readers and writers
  Atomic::add(&_used_generations[(int)id], size, memory_order_relaxed);
}

void ZPageAllocator::decrease_used_generation(ZGenerationId id, size_t size) {
  // Update atomically since we have concurrent readers and writers
  Atomic::sub(&_used_generations[(int)id], size, memory_order_relaxed);
}

void ZPageAllocator::promote_used(const ZPage* from, const ZPage* to) {
  assert(from->start() == to->start(), "pages start at same offset");
  assert(from->size() == to->size(),   "pages are the same size");
  assert(from->age() != ZPageAge::old, "must be promotion");
  assert(to->age() == ZPageAge::old,   "must be promotion");

  decrease_used_generation(ZGenerationId::young, to->size());
  increase_used_generation(ZGenerationId::old, to->size());
}

static void check_out_of_memory_during_initialization() {
  if (!is_init_completed()) {
    vm_exit_during_initialization("java.lang.OutOfMemoryError", "Java heap too small");
  }
}

ZPage* ZPageAllocator::alloc_page(ZPageType type, size_t size, ZAllocationFlags flags, ZPageAge age, uint32_t preferred_partition) {
  EventZPageAllocation event;

  ZPageAllocation allocation(type, size, flags, age, preferred_partition);

  // Allocate the page
  ZPage* const page = alloc_page_inner(&allocation, ZPageAllocationAttempt::initial);
  if (page == nullptr) {
    // Out of memory
    return nullptr;
  }

  // Update allocation statistics. Exclude gc relocations to avoid
  // artificial inflation of the allocation rate during relocation.
  if (!flags.gc_relocation() && is_init_completed()) {
    // Note that there are two allocation rate counters, which have
    // different purposes and are sampled at different frequencies.
    ZStatInc(ZCounterMutatorAllocationRate, page->size());
    ZStatMutatorAllocRate::sample_allocation(page->size());
  }

  const ZPageAllocationStats stats = allocation.stats();
  const int num_harvested_vmems = stats._num_harvested_vmems;
  const size_t harvested = stats._total_harvested;

  if (harvested > 0) {
    ZStatInc(ZCounterMappedCacheHarvest, harvested);
    log_debug(gc, heap)("Mapped Cache Harvested: %zuM (%d)", harvested / M, num_harvested_vmems);
  }

  // Send event for successful allocation
  allocation.send_event(true /* successful */);

  return page;
}

bool ZPageAllocator::alloc_page_stall(ZPageAllocation* allocation) {
  ZStatTimer timer(ZCriticalPhaseAllocationStall);
  EventZAllocationStall event;

  // We can only block if the VM is fully initialized
  check_out_of_memory_during_initialization();

  // Start asynchronous minor GC
  const ZDriverRequest request(GCCause::_z_allocation_stall, ZYoungGCThreads, 0);
  ZDriver::minor()->collect(request);

  // Wait for allocation to complete or fail
  const bool result = allocation->wait();

  {
    // Guard deletion of underlying semaphore. This is a workaround for
    // a bug in sem_post() in glibc < 2.21, where it's not safe to destroy
    // the semaphore immediately after returning from sem_wait(). The
    // reason is that sem_post() can touch the semaphore after a waiting
    // thread have returned from sem_wait(). To avoid this race we are
    // forcing the waiting thread to acquire/release the lock held by the
    // posting thread. https://sourceware.org/bugzilla/show_bug.cgi?id=12674
    ZLocker<ZLock> locker(&_lock);
  }

  // Send event
  event.commit((u8)allocation->type(), allocation->size());

  return result;
}

ZPage* ZPageAllocator::alloc_page_inner(ZPageAllocation* allocation, ZPageAllocationAttempt attempt) {
  // Claim the capacity needed for this allocation.
  //
  // The claimed capacity comes from memory already mapped in the cache, or
  // from increasing the capacity. The increased capacity allows us to allocate
  // physical memory from the physical memory manager later on.
  //
  // Note that this call might block in a safepoint if the non-blocking flag is
  // not set.
  if (!claim_capacity_or_stall(allocation, &attempt)) {
    // Out of memory
    return nullptr;
  }

  // If the entire claimed capacity came from claiming a single vmem from the
  // mapped cache then the allocation has been satisfied and we are done.
  const ZVirtualMemory cached_vmem = satisfied_from_cache_vmem(allocation);
  if (!cached_vmem.is_null()) {
    return create_page(allocation, cached_vmem);
  }

  // We couldn't find a satisfying vmem in the cache, so we need to build one.

  // Claim virtual memory, either from remapping harvested vmems from the
  // mapped cache or by claiming it straight from the virtual memory manager.
  const ZVirtualMemory vmem = claim_virtual_memory(allocation);
  if (vmem.is_null()) {
    log_error(gc)("Out of address space");
    free_after_alloc_page_failed(allocation);

    // Crash in debug builds for more information
    DEBUG_ONLY(fatal("Out of address space");)
    return nullptr;
  }

  // Claim physical memory for the increased capacity. The previous claiming of
  // capacity guarantees that this will succeed.
  claim_physical_for_increased_capacity(allocation, vmem);

  // Commit memory for the increased capacity and map the entire vmem.
  if (!commit_and_map(allocation, vmem)) {
    free_after_alloc_page_failed(allocation);
    assert(attempt != ZPageAllocationAttempt::retry, "Should be retry or stall");
    return alloc_page_inner(allocation, ZPageAllocationAttempt::retry);
  }

  return create_page(allocation, vmem);
}

bool ZPageAllocator::claim_capacity_or_stall(ZPageAllocation* allocation, ZPageAllocationAttempt* attempt) {
  {
    ZLocker<ZLock> locker(&_lock);

    // Try to claim memory
    if (claim_capacity(allocation, *attempt)) {
      // Keep track of usage
      increase_used(allocation->size());

      return true;
    }

    // Failed to claim memory
    if (allocation->flags().non_blocking()) {
      // Don't stall
      return false;
    }

    // Enqueue allocation request
    _stalled.insert_last(allocation);
  }

  // We are stalling on this allocation
  *attempt = ZPageAllocationAttempt::stall;

  // Stall
  return alloc_page_stall(allocation);
}

bool ZPageAllocator::claim_capacity(ZPageAllocation* allocation, ZPageAllocationAttempt attempt) {
  // Fast medium allocation
  if (allocation->flags().fast_medium()) {
    return claim_capacity_fast_medium(allocation);
  }
  const uint32_t start_numa_id = allocation->preferred_partition();
  const uint32_t start_partition = start_numa_id;
  const uint32_t num_partitions = _partitions.count();

  // Round robin soft single-partition claiming
  const size_t soft_limit = heuristic_max_capacity();

  uint32_t lowest_capacity_id = num_partitions;
  size_t lowest_capacity = std::numeric_limits<size_t>::max();

  for (uint32_t i = 0; i < num_partitions; ++i) {
    const uint32_t partition_id = (start_partition + i) % num_partitions;

    const size_t soft_partition_limit = ZNUMA::calculate_share(partition_id, soft_limit);
    if (claim_capacity_single_partition(allocation->single_partition_allocation(), partition_id, attempt, soft_partition_limit)) {
      return true;
    }

    size_t partition_capacity = _partitions.get(partition_id).capacity();
    if (partition_capacity < lowest_capacity) {
      lowest_capacity_id = partition_id;
      lowest_capacity = partition_capacity;
    }
  }

  // Hard single-partition claiming - start from lowest capacity partition
  // TODO: This overrides the soft_limit, should it be respected?
  const size_t hard_partition_limit = _partitions.get(lowest_capacity_id).static_max_capacity();
  if (claim_capacity_single_partition(allocation->single_partition_allocation(), lowest_capacity_id, attempt, hard_partition_limit)) {
    return true;
  }

  // TODO: This overrides the soft_limit, should it be respected?
  if (!is_multi_partition_allowed(allocation, attempt, _static_max_capacity)) {
    // Multi-partition claiming is not possible
    return false;
  }

  // Multi-partition claiming

  // Flip allocation to multi-partition allocation
  allocation->initiate_multi_partition_allocation();

  ZMultiPartitionAllocation* const multi_partition_allocation = allocation->multi_partition_allocation();

  claim_capacity_multi_partition(multi_partition_allocation, lowest_capacity_id, attempt, hard_partition_limit);

  return true;
}

bool ZPageAllocator::claim_capacity_fast_medium(ZPageAllocation* allocation) {
  const uint32_t start_node = allocation->preferred_partition();
  const uint32_t numa_nodes = ZNUMA::count();

  // Round robin soft single-partition claiming
  const size_t soft_limit = heuristic_max_capacity();

  const uint32_t num_partitions = _partitions.count();
  uint32_t lowest_capacity_id = num_partitions;
  size_t lowest_capacity = std::numeric_limits<size_t>::max();

  for (uint32_t i = 0; i < numa_nodes; ++i) {
    const uint32_t numa_id = (start_node + i) % numa_nodes;
    ZPartition& partition = _partitions.get(numa_id);
    ZSinglePartitionAllocation* single_partition_allocation = allocation->single_partition_allocation();

    const size_t soft_partition_limit = ZNUMA::calculate_share(numa_id, soft_limit);

    if (partition.claim_capacity_fast_medium(single_partition_allocation->allocation(), soft_partition_limit)) {
      return true;
    }

    size_t partition_capacity = _partitions.get(numa_id).capacity();
    if (partition_capacity < lowest_capacity) {
      lowest_capacity_id = numa_id;
      lowest_capacity = partition_capacity;
    }
  }

  // Hard single-partition claiming - start from lowest capacity partition
  ZSinglePartitionAllocation* single_partition_allocation = allocation->single_partition_allocation();
  // TODO: This overrides the soft_limit, should it be respected?
  const size_t hard_partition_limit = _partitions.get(lowest_capacity_id).static_max_capacity();
  return _partitions.get(lowest_capacity_id).claim_capacity_fast_medium(single_partition_allocation->allocation(), hard_partition_limit);
}

bool ZPageAllocator::claim_capacity_single_partition(ZSinglePartitionAllocation* single_partition_allocation, uint32_t partition_id, ZPageAllocationAttempt attempt, size_t limit) {
  ZPartition& partition = _partitions.get(partition_id);

  return partition.claim_capacity(single_partition_allocation->allocation(), attempt, limit);
}

void ZPageAllocator::claim_capacity_multi_partition(ZMultiPartitionAllocation* multi_partition_allocation, uint32_t start_partition, ZPageAllocationAttempt attempt, size_t limit) {
  const size_t size = multi_partition_allocation->size();
  const uint32_t num_partitions = _partitions.count();
  const size_t split_size = align_up(size / num_partitions, ZGranuleSize);

  size_t remaining = size;

  const auto do_claim_one_partition = [&](ZPartition& partition, bool claim_evenly) {
    if (remaining == 0) {
      // All memory claimed
      return false;
    }

    const size_t max_alloc_size = claim_evenly ? MIN2(split_size, remaining) : remaining;

    // This guarantees that claim_physical below will succeed
    const size_t alloc_size = MIN2(max_alloc_size, partition.available(attempt, limit));

    // Skip over empty allocations
    if (alloc_size == 0) {
      // Continue
      return true;
    }

    ZMemoryAllocation partial_allocation(alloc_size);

    // Claim capacity for this allocation - this should succeed
    const bool result = partition.claim_capacity(&partial_allocation, attempt, limit);
    assert(result, "Should have succeeded");

    // Register allocation
    multi_partition_allocation->register_allocation(partial_allocation);

    // Update remaining
    remaining -= alloc_size;

    // Continue
    return true;
  };

  // Loops over every partition and claims memory
  const auto do_claim_each_partition = [&](bool claim_evenly) {
    for (uint32_t i = 0; i < num_partitions; ++i) {
      const uint32_t partition_id = (start_partition + i) % num_partitions;
      ZPartition& partition = _partitions.get(partition_id);

      if (!do_claim_one_partition(partition, claim_evenly)) {
        // All memory claimed
        break;
      }
    }
  };

  // Try to claim from multiple partitions

  // Try to claim up to split_size on each partition
  do_claim_each_partition(true  /* claim_evenly */);

  // Try claim the remaining
  do_claim_each_partition(false /* claim_evenly */);

  assert(remaining == 0, "Must have claimed capacity for the whole allocation");
}

ZVirtualMemory ZPageAllocator::satisfied_from_cache_vmem(const ZPageAllocation* allocation) const {
  if (allocation->is_multi_partition()) {
    // Multi-partition allocations are always harvested and/or committed, so
    // there's never a satisfying vmem from the caches.
    return {};
  }

  return allocation->satisfied_from_cache_vmem();
}

ZVirtualMemory ZPageAllocator::claim_virtual_memory(ZPageAllocation* allocation) {
  // Note: that the single-partition performs "shuffling" of already harvested
  // vmem(s), while the multi-partition searches for available virtual memory
  // area without shuffling.

  if (allocation->is_multi_partition()) {
    return claim_virtual_memory_multi_partition(allocation->multi_partition_allocation());
  } else {
    return claim_virtual_memory_single_partition(allocation->single_partition_allocation());
  }
}

ZVirtualMemory ZPageAllocator::claim_virtual_memory_single_partition(ZSinglePartitionAllocation* single_partition_allocation) {
  ZMemoryAllocation* const allocation = single_partition_allocation->allocation();
  ZPartition& partition = allocation->partition();

  if (allocation->harvested() > 0) {
    // We claim virtual memory from the harvested vmems and perhaps also
    // allocate more to match the allocation request.
    return partition.prepare_harvested_and_claim_virtual(allocation);
  } else {
    // Just try to claim virtual memory
    return partition.claim_virtual(allocation->size());
  }
}

ZVirtualMemory ZPageAllocator::claim_virtual_memory_multi_partition(ZMultiPartitionAllocation* multi_partition_allocation) {
  const size_t size = multi_partition_allocation->size();

  const ZVirtualMemory vmem = _virtual.remove_from_low_multi_partition(size);
  if (!vmem.is_null()) {
    // Copy claimed multi-partition vmems, we leave the old vmems mapped until
    // after we have committed. In case committing fails we can simply
    // reinsert the initial vmems.
    copy_claimed_physical_multi_partition(multi_partition_allocation, vmem);
  }

  return vmem;
}

void ZPageAllocator::copy_claimed_physical_multi_partition(ZMultiPartitionAllocation* multi_partition_allocation, const ZVirtualMemory& vmem) {
  // Start at the new dest offset
  ZVirtualMemory remaining_dest_vmem = vmem;

  for (const ZMemoryAllocation* partial_allocation : *multi_partition_allocation->allocations()) {
    // Split off the partial allocation's destination vmem
    ZVirtualMemory partial_dest_vmem = remaining_dest_vmem.shrink_from_front(partial_allocation->size());

    // Get the partial allocation's partition
    ZPartition& partition = partial_allocation->partition();

    // Copy all physical segments from the partition to the destination vmem
    for (const ZVirtualMemory from_vmem : *partial_allocation->partial_vmems()) {
      // Split off destination
      const ZVirtualMemory to_vmem = partial_dest_vmem.shrink_from_front(from_vmem.size());

      // Copy physical segments
      partition.copy_physical_segments_from_partition(from_vmem, to_vmem);
    }
  }
}

void ZPageAllocator::claim_physical_for_increased_capacity(ZPageAllocation* allocation, const ZVirtualMemory& vmem) {
  assert(allocation->size() == vmem.size(), "vmem should be the final entry");

  if (allocation->is_multi_partition()) {
    claim_physical_for_increased_capacity_multi_partition(allocation->multi_partition_allocation(), vmem);
  } else {
    claim_physical_for_increased_capacity_single_partition(allocation->single_partition_allocation(), vmem);
  }
}

void ZPageAllocator::claim_physical_for_increased_capacity_single_partition(ZSinglePartitionAllocation* single_partition_allocation, const ZVirtualMemory& vmem) {
  claim_physical_for_increased_capacity(single_partition_allocation->allocation(), vmem);
}

void ZPageAllocator::claim_physical_for_increased_capacity_multi_partition(const ZMultiPartitionAllocation* multi_partition_allocation, const ZVirtualMemory& vmem) {
  ZVirtualMemory remaining = vmem;

  for (ZMemoryAllocation* allocation : *multi_partition_allocation->allocations()) {
    const ZVirtualMemory partial = remaining.shrink_from_front(allocation->size());
    claim_physical_for_increased_capacity(allocation, partial);
  }
}

void ZPageAllocator::claim_physical_for_increased_capacity(ZMemoryAllocation* allocation, const ZVirtualMemory& vmem) {
  // The previously harvested memory is memory that has already been committed
  // and mapped. The rest of the vmem gets physical memory assigned here and
  // will be committed in a subsequent function.

  const size_t already_committed = allocation->harvested();
  const size_t non_committed = allocation->size() - already_committed;
  const size_t increased_capacity = allocation->increased_capacity();

  assert(non_committed == increased_capacity,
         "Mismatch non_committed: " PTR_FORMAT " increased_capacity: " PTR_FORMAT,
         non_committed, increased_capacity);

  if (non_committed > 0) {
    ZPartition& partition = allocation->partition();
    ZVirtualMemory non_committed_vmem = vmem.last_part(already_committed);
    partition.claim_physical(non_committed_vmem);
  }
}

bool ZPageAllocator::commit_and_map(ZPageAllocation* allocation, const ZVirtualMemory& vmem) {
  assert(allocation->size() == vmem.size(), "vmem should be the final entry");

  if (allocation->is_multi_partition()) {
    return commit_and_map_multi_partition(allocation->multi_partition_allocation(), vmem);
  } else {
    return commit_and_map_single_partition(allocation->single_partition_allocation(), vmem);
  }
}

bool ZPageAllocator::commit_and_map_single_partition(ZSinglePartitionAllocation* single_partition_allocation, const ZVirtualMemory& vmem) {
  const bool commit_successful = commit_single_partition(single_partition_allocation, vmem);

  // Map the vmem
  map_committed_single_partition(single_partition_allocation, vmem);

  if (commit_successful) {
    return true;
  }

  // Commit failed
  cleanup_failed_commit_single_partition(single_partition_allocation, vmem);

  return false;
}

bool ZPageAllocator::commit_and_map_multi_partition(ZMultiPartitionAllocation* multi_partition_allocation, const ZVirtualMemory& vmem) {
  if (commit_multi_partition(multi_partition_allocation, vmem)) {
    // Commit successful

    // Unmap harvested vmems
    unmap_harvested_multi_partition(multi_partition_allocation);

    // Map the vmem
    map_committed_multi_partition(multi_partition_allocation, vmem);

    return true;
  }

  // Commit failed
  cleanup_failed_commit_multi_partition(multi_partition_allocation, vmem);

  return false;
}

void ZPageAllocator::commit(ZMemoryAllocation* allocation, const ZVirtualMemory& vmem) {
  ZPartition& partition = allocation->partition();

  if (allocation->increased_capacity() > 0) {
    // Commit memory
    partition.commit_increased_capacity(allocation, vmem);
  }
}

bool ZPageAllocator::commit_single_partition(ZSinglePartitionAllocation* single_partition_allocation, const ZVirtualMemory& vmem) {
  ZMemoryAllocation* const allocation = single_partition_allocation->allocation();

  commit(allocation, vmem);

  return !allocation->commit_failed();
}

bool ZPageAllocator::commit_multi_partition(ZMultiPartitionAllocation* multi_partition_allocation, const ZVirtualMemory& vmem) {
  bool commit_failed = false;
  ZVirtualMemory remaining = vmem;
  for (ZMemoryAllocation* const allocation : *multi_partition_allocation->allocations()) {
    // Split off the partial allocation's memory range
    const ZVirtualMemory partial_vmem = remaining.shrink_from_front(allocation->size());

    commit(allocation, partial_vmem);

    // Keep track if any partial allocation failed to commit
    commit_failed |= allocation->commit_failed();
  }

  assert(remaining.size() == 0, "all memory must be accounted for");

  return !commit_failed;
}

void ZPageAllocator::unmap_harvested_multi_partition(ZMultiPartitionAllocation* multi_partition_allocation) {
  for (ZMemoryAllocation* const allocation : *multi_partition_allocation->allocations()) {
    ZPartition& partition = allocation->partition();
    ZArray<ZVirtualMemory>* const partial_vmems = allocation->partial_vmems();

    // Unmap harvested vmems
    while (!partial_vmems->is_empty()) {
      const ZVirtualMemory to_unmap = partial_vmems->pop();
      partition.unmap_virtual(to_unmap);
      partition.free_virtual(to_unmap);
    }
  }
}

void ZPageAllocator::map_committed_single_partition(ZSinglePartitionAllocation* single_partition_allocation, const ZVirtualMemory& vmem) {
  ZMemoryAllocation* const allocation = single_partition_allocation->allocation();
  ZPartition& partition = allocation->partition();

  const size_t total_committed = allocation->harvested() + allocation->committed_capacity();

  if (total_committed > 0)  {
    // Map all the committed memory
    const ZVirtualMemory total_committed_vmem = vmem.first_part(total_committed);
    partition.map_memory(allocation, total_committed_vmem);
  }
}

void ZPageAllocator::map_committed_multi_partition(ZMultiPartitionAllocation* multi_partition_allocation, const ZVirtualMemory& vmem) {
  ZVirtualMemory remaining = vmem;
  for (ZMemoryAllocation* const allocation : *multi_partition_allocation->allocations()) {
    assert(!allocation->commit_failed(), "Sanity check");

    ZPartition& partition = allocation->partition();

    // Split off the partial allocation's memory range
    const ZVirtualMemory to_vmem = remaining.shrink_from_front(allocation->size());

    // Map the partial_allocation to partial_vmem
    partition.map_virtual_from_multi_partition(to_vmem);
  }

  assert(remaining.size() == 0, "all memory must be accounted for");
}

void ZPageAllocator::cleanup_failed_commit_single_partition(ZSinglePartitionAllocation* single_partition_allocation, const ZVirtualMemory& vmem) {
  ZMemoryAllocation* const allocation = single_partition_allocation->allocation();

  assert(allocation->commit_failed(), "Must have failed to commit");
  assert(allocation->partial_vmems()->is_empty(), "Invariant for single partition commit failure");

  // For a single partition we have unmapped the harvested memory before we
  // started committing, and moved its physical memory association to the start
  // of the vmem. As such, the partial_vmems is empty. All the harvested and
  // partially successfully committed memory is mapped in the first part of vmem.
  const size_t harvested_and_committed_capacity = allocation->harvested() + allocation->committed_capacity();
  const ZVirtualMemory succeeded_vmem = vmem.first_part(harvested_and_committed_capacity);
  const ZVirtualMemory failed_vmem = vmem.last_part(harvested_and_committed_capacity);

  if (succeeded_vmem.size() > 0) {
    // Register the committed and mapped memory. We insert the committed
    // memory into partial_vmems so that it will be inserted into the cache
    // in a subsequent step.
    allocation->partial_vmems()->append(succeeded_vmem);
  }

  // Free the virtual and physical memory we fetched to use but failed to commit
  ZPartition& partition = allocation->partition();
  partition.free_physical(failed_vmem);
  partition.free_virtual(failed_vmem);
}

void ZPageAllocator::cleanup_failed_commit_multi_partition(ZMultiPartitionAllocation* multi_partition_allocation, const ZVirtualMemory& vmem) {
  ZVirtualMemory remaining = vmem;
  for (ZMemoryAllocation* const allocation : *multi_partition_allocation->allocations()) {
    // Split off the partial allocation's memory range
    const ZVirtualMemory partial_vmem = remaining.shrink_from_front(allocation->size());

    if (allocation->harvested() == allocation->size()) {
      // Everything is harvested, the mappings are already in the partial_vmems,
      // nothing to cleanup.
      continue;
    }

    const size_t committed = allocation->committed_capacity();
    const ZVirtualMemory non_harvested_vmem = partial_vmem.last_part(allocation->harvested());
    const ZVirtualMemory committed_vmem = non_harvested_vmem.first_part(committed);
    const ZVirtualMemory non_committed_vmem = non_harvested_vmem.last_part(committed);

    ZPartition& partition = allocation->partition();

    if (allocation->commit_failed()) {
      // Free the physical memory we failed to commit. Virtual memory is later
      // freed for the entire multi-partition allocation after all memory
      // allocations have been visited.
      partition.free_physical(non_committed_vmem);
    }

    if (committed_vmem.size() == 0) {
      // Nothing committed, nothing more to cleanup
      continue;
    }

    ZArray<ZVirtualMemory>* const partial_vmems = allocation->partial_vmems();

    // Keep track of the start index
    const int start_index = partial_vmems->length();

    // Claim virtual memory for the committed part
    const size_t claimed_virtual = partition.claim_virtual(committed, partial_vmems);

    // We are holding memory associated with this partition, and we do not
    // overcommit virtual memory claiming. So virtual memory must always be
    // available.
    assert(claimed_virtual == committed, "must succeed");

    // Associate and map the physical memory with the partial vmems

    ZVirtualMemory remaining_committed_vmem = committed_vmem;
    for (const ZVirtualMemory& to_vmem : partial_vmems->slice_back(start_index)) {
      const ZVirtualMemory from_vmem = remaining_committed_vmem.shrink_from_front(to_vmem.size());

      // Copy physical mappings
      partition.copy_physical_segments_to_partition(to_vmem, from_vmem);

      // Map memory
      partition.map_virtual(to_vmem);
    }

    assert(remaining_committed_vmem.size() == 0, "all memory must be accounted for");
  }

  assert(remaining.size() == 0, "all memory must be accounted for");

  // Free the unused virtual memory
  _virtual.insert_multi_partition(vmem);
}

void ZPageAllocator::truncate_heuristic_max_after_commit_failure() {
  // Adjust heuristic max capacity to ensure GC tries to keep below current capacity
  const size_t cap = capacity();
  for (;;) {
    const size_t heuristic_max = heuristic_max_capacity();
    if (heuristic_max > cap) {
      if (Atomic::cmpxchg(&_heuristic_max_capacity, heuristic_max, cap) != heuristic_max) {
        continue;
      }
      const size_t current_max_cap = current_max_capacity();
      log_debug(gc)("Forced to lower heap size from "
                    "%zuM(%.0f%%) to %zuM(%.0f%%)",
                    heuristic_max / M, percent_of(heuristic_max, current_max_cap),
                    cap / M, percent_of(cap, current_max_cap));
      heap_truncated(heuristic_max);
    }
    return;
  }
}

void ZPageAllocator::free_after_alloc_page_failed(ZPageAllocation* allocation) {
  // Send event for failed allocation
  allocation->send_event(false /* successful */);

  ZLocker<ZLock> locker(&_lock);

  // Free memory
  free_memory_alloc_failed(allocation);

  // Try not to commit too much again
  truncate_heuristic_max_after_commit_failure();

  // Keep track of usage
  decrease_used(allocation->size());

  // Reset allocation for a potential retry
  allocation->reset_for_retry();

  // Try satisfy stalled allocations
  satisfy_stalled();
}

void ZPageAllocator::free_memory_alloc_failed(ZPageAllocation* allocation) {
  if (allocation->is_multi_partition()) {
    free_memory_alloc_failed_multi_partition(allocation->multi_partition_allocation());
  } else {
    free_memory_alloc_failed_single_partition(allocation->single_partition_allocation());
  }
}

void ZPageAllocator::free_memory_alloc_failed_single_partition(ZSinglePartitionAllocation* single_partition_allocation) {
  free_memory_alloc_failed(single_partition_allocation->allocation());
}

void ZPageAllocator::free_memory_alloc_failed_multi_partition(ZMultiPartitionAllocation* multi_partition_allocation) {
  for (ZMemoryAllocation* allocation : *multi_partition_allocation->allocations()) {
    free_memory_alloc_failed(allocation);
  }
}

void ZPageAllocator::free_memory_alloc_failed(ZMemoryAllocation* allocation) {
  ZPartition& partition = allocation->partition();

  partition.free_memory_alloc_failed(allocation);
}

ZPage* ZPageAllocator::create_page(ZPageAllocation* allocation, const ZVirtualMemory& vmem) {
  assert(allocation->size() == vmem.size(), "Must be %zu == %zu", allocation->size(), vmem.size());

  // We don't track generation usage when claiming capacity, because this page
  // could have been allocated by a thread that satisfies a stalling allocation.
  // The stalled thread can wake up and potentially realize that the page alloc
  // should be undone. If the alloc and the undo gets separated by a safepoint,
  // the generation statistics could se a decreasing used value between mark
  // start and mark end. At this point an allocation will be successful, so we
  // update the generation usage.
  const ZGenerationId id = allocation->age() == ZPageAge::old ? ZGenerationId::old : ZGenerationId::young;
  increase_used_generation(id, allocation->size());

  const ZPageType type = allocation->type();
  const ZPageAge age = allocation->age();

  if (allocation->is_multi_partition()) {
    const ZMultiPartitionAllocation* const multi_partition_allocation = allocation->multi_partition_allocation();
    ZMultiPartitionTracker* const tracker = ZMultiPartitionTracker::create(multi_partition_allocation, vmem);

    return new ZPage(type, age, vmem, tracker);
  }

  const ZSinglePartitionAllocation* const single_partition_allocation = allocation->single_partition_allocation();
  const uint32_t partition_id = single_partition_allocation->allocation()->partition().numa_id();

  return new ZPage(type, age, vmem, partition_id);
}

void ZPageAllocator::prepare_memory_for_free(ZPage* page, ZArray<ZVirtualMemory>* vmems) {
  // Extract memory and destroy the page
  const ZVirtualMemory vmem = page->virtual_memory();
  const ZPageType page_type = page->type();
  const ZMultiPartitionTracker* const tracker = page->multi_partition_tracker();

  safe_destroy_page(page);

  // Multi-partition memory is always remapped
  if (tracker != nullptr) {
    tracker->prepare_memory_for_free(vmem, vmems);

    // Free the virtual memory
    _virtual.insert_multi_partition(vmem);

    // Destroy the tracker
    ZMultiPartitionTracker::destroy(tracker);
    return;
  }

  // Try to remap and defragment if page is large
  if (page_type == ZPageType::large) {
    remap_and_defragment(vmem, vmems);
    return;
  }

  // Leave the memory untouched
  vmems->append(vmem);
}

void ZPageAllocator::remap_and_defragment(const ZVirtualMemory& vmem, ZArray<ZVirtualMemory>* vmems_out) {
  ZPartition& partition = partition_from_vmem(vmem);

  // If no lower address can be found, don't remap/defrag
  if (_virtual.lowest_available_address(partition.numa_id()) > vmem.start()) {
    vmems_out->append(vmem);
    return;
  }

  ZStatInc(ZCounterDefragment);

  // Synchronously unmap the virtual memory
  partition.unmap_virtual(vmem);

  // Stash segments
  ZArray<zbacking_index> stash(vmem.granule_count());
  _physical.stash_segments(vmem, &stash);

  // Shuffle vmem - put new vmems in vmems_out
  const int start_index = vmems_out->length();
  partition.free_and_claim_virtual_from_low_many(vmem, vmems_out);

  // The output array may contain results from other defragmentations as well,
  // so we only operate on the result(s) we just got.
  ZArraySlice<ZVirtualMemory> defragmented_vmems = vmems_out->slice_back(start_index);

  // Restore segments
  _physical.restore_segments(defragmented_vmems, stash);

  // Map and pre-touch
  for (const ZVirtualMemory& claimed_vmem : defragmented_vmems) {
    partition.map_virtual(claimed_vmem);
    pretouch_memory(claimed_vmem.start(), claimed_vmem.size());
  }
}

void ZPageAllocator::free_memory(ZArray<ZVirtualMemory>* vmems) {
  ZLocker<ZLock> locker(&_lock);

  // Free the vmems
  for (const ZVirtualMemory vmem : *vmems) {
    ZPartition& partition = partition_from_vmem(vmem);

    // Free the vmem
    partition.free_memory(vmem);

    // Keep track of usage
    decrease_used(vmem.size());
  }

  // Try satisfy stalled allocations
  satisfy_stalled();
}

void ZPageAllocator::satisfy_stalled() {
  for (;;) {
    ZPageAllocation* const allocation = _stalled.first();
    if (allocation == nullptr) {
      // Allocation queue is empty
      return;
    }

    if (!claim_capacity(allocation, ZPageAllocationAttempt::stall)) {
      // Allocation could not be satisfied, give up
      return;
    }

    // Keep track of usage
    increase_used(allocation->size());

    // Allocation succeeded, dequeue and satisfy allocation request.
    // Note that we must dequeue the allocation request first, since
    // it will immediately be deallocated once it has been satisfied.
    _stalled.remove(allocation);
    allocation->satisfy(true);
  }
}

bool ZPageAllocator::is_multi_partition_enabled() const {
  return _virtual.is_multi_partition_enabled();
}

bool ZPageAllocator::is_multi_partition_allowed(const ZPageAllocation* allocation, ZPageAllocationAttempt attempt, size_t total_limit) const {
  return is_multi_partition_enabled() &&
         allocation->type() == ZPageType::large &&
         allocation->size() <= sum_available(attempt, total_limit);
}

const ZPartition& ZPageAllocator::partition_from_partition_id(uint32_t numa_id) const {
  return _partitions.get(numa_id);
}

ZPartition& ZPageAllocator::partition_from_partition_id(uint32_t numa_id) {
  return _partitions.get(numa_id);
}

ZPartition& ZPageAllocator::partition_from_vmem(const ZVirtualMemory& vmem) {
  return partition_from_partition_id(_virtual.lookup_partition_id(vmem));
}

size_t ZPageAllocator::sum_available(ZPageAllocationAttempt attempt, size_t total_limit) const {
  size_t total = 0;

  ZPartitionConstIterator iter = partition_iterator();
  for (const ZPartition* partition; iter.next(&partition);) {
    if (total_limit <= total) {
      // The limit smaller than the total, we will have
      return total_limit;
    }
    const size_t partition_limit = MIN2(partition->static_max_capacity(), total_limit - total);
    total += partition->available(attempt, partition_limit);
  }

  return total;
}

void ZPageAllocator::increase_used(size_t size) {
  // Update atomically since we have concurrent readers
  const size_t used = Atomic::add(&_used, size);

  // Update used high
  for (auto& stats : _collection_stats) {
    if (used > stats._used_high) {
      stats._used_high = used;
    }
  }
}

void ZPageAllocator::decrease_used(size_t size) {
  // Update atomically since we have concurrent readers
  const size_t used = Atomic::sub(&_used, size);

  // Update used low
  for (auto& stats : _collection_stats) {
    if (used < stats._used_low) {
      stats._used_low = used;
    }
  }
}

void ZPageAllocator::safe_destroy_page(ZPage* page) {
  // Destroy page safely
  _safe_destroy.schedule_delete(page);
}

void ZPageAllocator::free_page(ZPage* page) {
  // Extract the id from the page
  const ZGenerationId id = page->generation_id();
  const size_t size = page->size();

  // Extract vmems and destroy the page
  ZArray<ZVirtualMemory> vmems;
  prepare_memory_for_free(page, &vmems);

  // Updated used statistics
  decrease_used_generation(id, size);

  // Free the extracted vmems
  free_memory(&vmems);
}

void ZPageAllocator::free_pages(ZGenerationId id, const ZArray<ZPage*>* pages) {
  // Prepare memory from pages to be cached
  ZArray<ZVirtualMemory> vmems;
  for (ZPage* page : *pages) {
    assert(page->generation_id() == id, "All pages must be from the same generation");
    const size_t size = page->size();

    // Extract vmems and destroy the page
    prepare_memory_for_free(page, &vmems);

    // Updated used statistics
    decrease_used_generation(id, size);
  }

  // Free the extracted vmems
  free_memory(&vmems);
}

void ZPageAllocator::enable_safe_destroy() const {
  _safe_destroy.enable_deferred_delete();
}

void ZPageAllocator::disable_safe_destroy() const {
  _safe_destroy.disable_deferred_delete();
}

static bool has_alloc_seen_young(const ZPageAllocation* allocation) {
  return allocation->young_seqnum() != ZGeneration::young()->seqnum();
}

static bool has_alloc_seen_old(const ZPageAllocation* allocation) {
  return allocation->old_seqnum() != ZGeneration::old()->seqnum();
}

bool ZPageAllocator::is_alloc_stalling() const {
  ZLocker<ZLock> locker(&_lock);
  return _stalled.first() != nullptr;
}

bool ZPageAllocator::is_alloc_stalling_for_old() const {
  ZLocker<ZLock> locker(&_lock);

  ZPageAllocation* const allocation = _stalled.first();
  if (allocation == nullptr) {
    // No stalled allocations
    return false;
  }

  return has_alloc_seen_young(allocation) && !has_alloc_seen_old(allocation);
}

void ZPageAllocator::notify_out_of_memory() {
  // Fail allocation requests that were enqueued before the last major GC started
  for (ZPageAllocation* allocation = _stalled.first(); allocation != nullptr; allocation = _stalled.first()) {
    if (!has_alloc_seen_old(allocation)) {
      // Not out of memory, keep remaining allocation requests enqueued
      return;
    }

    // Out of memory, dequeue and fail allocation request
    _stalled.remove(allocation);
    allocation->satisfy(false);
  }
}

void ZPageAllocator::restart_gc() const {
  ZPageAllocation* const allocation = _stalled.first();
  if (allocation == nullptr) {
    // No stalled allocations
    return;
  }

  if (!has_alloc_seen_young(allocation)) {
    // Start asynchronous minor GC, keep allocation requests enqueued
    const ZDriverRequest request(GCCause::_z_allocation_stall, ZYoungGCThreads, 0);
    ZDriver::minor()->collect(request);
  } else {
    // Start asynchronous major GC, keep allocation requests enqueued
    const ZDriverRequest request(GCCause::_z_allocation_stall, ZYoungGCThreads, ZOldGCThreads);
    ZDriver::major()->collect(request);
  }
}

void ZPageAllocator::handle_alloc_stalling_for_young() {
  ZLocker<ZLock> locker(&_lock);
  restart_gc();
}

void ZPageAllocator::handle_alloc_stalling_for_old(bool cleared_all_soft_refs) {
  ZLocker<ZLock> locker(&_lock);
  if (cleared_all_soft_refs) {
    notify_out_of_memory();
  }
  restart_gc();
}

ZPartitionConstIterator ZPageAllocator::partition_iterator() const {
  return ZPartitionConstIterator(&_partitions);
}

ZPartitionIterator ZPageAllocator::partition_iterator() {
  return ZPartitionIterator(&_partitions);
}

void ZPageAllocator::threads_do(ThreadClosure* tc) const {
  ZPartitionConstIterator iter = partition_iterator();
  for (const ZPartition* partition; iter.next(&partition);) {
    partition->threads_do(tc);
  }
}

static bool try_lock_on_error(ZLock* lock) {
  if (VMError::is_error_reported() && VMError::is_error_reported_in_current_thread()) {
    return lock->try_lock();
  }

  lock->lock();

  return true;
}

void ZPageAllocator::print_usage_on(outputStream* st) const {
  const bool locked = try_lock_on_error(&_lock);

  if (!locked) {
    st->print_cr("<Without lock>");
  }

  // Print information even though we may not have successfully taken the lock.
  // This is thread-safe, but may produce inconsistent results.

  print_total_usage_on(st);

  StreamIndentor si(st, 1);
  print_partition_usage_on(st);

  if (locked) {
    _lock.unlock();
  }
}

void ZPageAllocator::print_total_usage_on(outputStream* st) const {
  st->print("ZHeap ");
  st->fill_to(17);
  st->print_cr("used %zuM, capacity %zuM, max capacity %zuM",
               used() / M, capacity() / M, dynamic_max_capacity() / M);
}

void ZPageAllocator::print_partition_usage_on(outputStream* st) const {
  if (_partitions.count() == 1) {
    // Partition usage is redundant if we only have one partition. Only
    // print the cache.
    _partitions.get(0).print_cache_on(st);
    return;
  }

  // Print all partitions
  ZPartitionConstIterator iter = partition_iterator();
  for (const ZPartition* partition; iter.next(&partition);) {
    partition->print_on(st);
  }
}

void ZPageAllocator::print_cache_extended_on(outputStream* st) const {
  st->print_cr("ZMappedCache:");

  StreamIndentor si(st, 1);

  if (!try_lock_on_error(&_lock)) {
    // We can't print without taking the lock since printing the contents of
    // the cache requires iterating over the nodes in the cache's tree, which
    // is not thread-safe.
    st->print_cr("<Skipped>");

    return;
  }

  // Print each partition's cache content
  ZPartitionConstIterator iter = partition_iterator();
  for (const ZPartition* partition; iter.next(&partition);) {
    partition->print_cache_extended_on(st);
  }

  _lock.unlock();
}
