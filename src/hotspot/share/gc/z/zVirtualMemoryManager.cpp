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

#include "gc/shared/gc_globals.hpp"
#include "gc/shared/gcLogPrecious.hpp"
#include "gc/z/zAddress.inline.hpp"
#include "gc/z/zAddressSpaceLimit.hpp"
#include "gc/z/zGlobals.hpp"
#include "gc/z/zInitialize.hpp"
#include "gc/z/zMemory.inline.hpp"
#include "gc/z/zNMT.hpp"
#include "gc/z/zNUMA.inline.hpp"
#include "gc/z/zValue.inline.hpp"
#include "utilities/align.hpp"
#include "utilities/debug.hpp"

class ZVirtualReservation {
private:
  ZMemoryManager _reserved;
  size_t _size;
  size_t _min_range;

  static size_t limit_max_reservation(size_t size) {
    const size_t limit = MIN2(ZAddressOffsetMax, ZAddressSpaceLimit::heap());
    return MIN2(size * ZVirtualToPhysicalRatio, limit);
  }

  static size_t calculate_min_range(size_t size) {
    // Don't try to reserve address ranges smaller than 1% of the requested size.
    // This avoids an explosion of reservation attempts in case large parts of the
    // address space is already occupied.
    return align_up(size / ZMaxVirtualReservations, ZGranuleSize);
  }

public:
  ZVirtualReservation(size_t size)
    : _reserved(),
      _size(limit_max_reservation(size)),
      _min_range(calculate_min_range(size)) {
    assert(is_aligned(size, ZGranuleSize), "Must be granule aligned 0x%zx", size);
  }

  void reserve(zoffset start, size_t size) {
    _reserved.free(start, size);
  }

  void transfer(ZMemoryManager& other, size_t size) {
    _reserved.transfer_low_address(other, size);
  }

  size_t size() const {
    return _size;
  }

  bool set_size(size_t size) {
    assert(is_aligned(size, ZGranuleSize), "Misaligned");
    if (size < _min_range) {
      return false;
    }

    _size = size;

    return true;
  }

  size_t min_range() const {
    return _min_range;
  }

  bool is_contiguous() const {
    return _reserved.free_is_contiguous();
  }
};

ZVirtualMemoryManager::ZVirtualMemoryManager(size_t max_capacity)
  : _reserved_memory(),
    _managers(),
    _vmem_ranges(),
    _initialized(false) {

  assert(max_capacity <= ZAddressOffsetMax, "Too large max_capacity");

  // Initialize platform specific parts before reserving address space
  pd_initialize_before_reserve();

  // Reserve address space
  ZVirtualReservation reservation(max_capacity);
  const size_t reserved_total = reserve(&reservation);
  if (reserved_total < max_capacity) {
    ZInitialize::error_d("Failed to reserve enough address space for Java heap");
    return;
  }

  // Initialize platform specific parts after reserving address space
  pd_initialize_after_reserve();

  // Install reserved memory into manager(s)
  ZNUMA::divide_resource(reserved_total, [&](int numa_id, size_t reserved) {
    ZMemoryManager& manager = _managers.get(numa_id);

    // Transfer reserved memory
    reservation.transfer(manager, reserved);

    // Store the range for the manager
    _vmem_ranges.set(manager.total_range(), numa_id);
  });

  // Successfully initialized
  _initialized = true;
}

#ifdef ASSERT
size_t ZVirtualMemoryManager::force_reserve_discontiguous(ZVirtualReservation* reservation) {
  const size_t size = reservation->size();
  const size_t max_range = MAX2(align_down(size / ZForceDiscontiguousHeapReservations, ZGranuleSize), reservation->min_range());
  size_t reserved = 0;

  // Try to reserve ZForceDiscontiguousHeapReservations number of virtual memory
  // ranges. Starting with higher addresses.
  uintptr_t end = ZAddressOffsetMax;
  while (reserved < size && end >= max_range) {
    const size_t remaining = size - reserved;
    const size_t reserve_size = MIN2(max_range, remaining);
    const uintptr_t reserve_start = end - reserve_size;

    reservation->set_size(reserve_size);
    if (reserve_contiguous(reservation, to_zoffset(reserve_start))) {
      reserved += reserve_size;
    }

    end -= reserve_size * 2;
  }

  // If (reserved < size) attempt to reserve the rest via normal divide and conquer
  uintptr_t start = 0;
  while (reserved < size && start < ZAddressOffsetMax) {
    const size_t remaining = MIN2(size - reserved, ZAddressOffsetMax - start);
    if (reservation->set_size(remaining)) {
      reserved += reserve_discontiguous(reservation, to_zoffset(start));
    }
    start += remaining;
  }

  return reserved;
}
#endif

size_t ZVirtualMemoryManager::reserve_discontiguous(ZVirtualReservation* reservation, zoffset start) {
  if (reserve_contiguous(reservation, start)) {
    return reservation->size();
  }

  const size_t size = reservation->size();
  const size_t half = size / 2;
  if (half < reservation->min_range()) {
    // Too small
    return 0;
  }

  // Divide and conquer
  const size_t first_part = align_down(half, ZGranuleSize);
  reservation->set_size(first_part);
  const size_t first_size = reserve_discontiguous(reservation, start);

  const size_t second_part = size - first_part;
  reservation->set_size(second_part);
  const size_t second_size = reserve_discontiguous(reservation, start + first_part);

  return first_size + second_size;
}

size_t ZVirtualMemoryManager::reserve_discontiguous(ZVirtualReservation* reservation) {
  const size_t size = reservation->size();
  uintptr_t start = 0;
  size_t reserved = 0;

  // Reserve size somewhere between [0, ZAddressOffsetMax)
  while (reserved < size && start < ZAddressOffsetMax) {
    const size_t remaining = MIN2(size - reserved, ZAddressOffsetMax - start);
    if (reservation->set_size(remaining)) {
      reserved += reserve_discontiguous(reservation, to_zoffset(start));
    }
    start += remaining;
  }

  return reserved;
}

bool ZVirtualMemoryManager::reserve_contiguous(ZVirtualReservation* reservation, zoffset start) {
  const size_t size = reservation->size();

  // Reserve address views
  const zaddress_unsafe addr = ZOffset::address_unsafe(start);

  // Reserve address space
  if (!pd_reserve(addr, size)) {
    return false;
  }

  // Register address views with native memory tracker
  ZNMT::reserve(addr, size);

  reservation->reserve(start, size);

  return true;
}

bool ZVirtualMemoryManager::reserve_contiguous(ZVirtualReservation* reservation) {
  // Allow at most 8192 attempts spread evenly across [0, ZAddressOffsetMax)
  const size_t size = reservation->size();
  const size_t unused = ZAddressOffsetMax - size;
  const size_t increment = MAX2(align_up(unused / 8192, ZGranuleSize), ZGranuleSize);

  for (uintptr_t start = 0; start + size <= ZAddressOffsetMax; start += increment) {
    if (reserve_contiguous(reservation, to_zoffset(start))) {
      // Success
      return true;
    }
  }

  // Failed
  return false;
}

size_t ZVirtualMemoryManager::reserve(ZVirtualReservation* reservation) {
  const size_t size = reservation->size();

  auto do_reserve = [&]() {
#ifdef ASSERT
    if (ZForceDiscontiguousHeapReservations > 0) {
      return force_reserve_discontiguous(reservation);
    }
#endif

    // Prefer a contiguous address space
    if (reserve_contiguous(reservation)) {
      return reservation->size();
    }

    // Fall back to a discontiguous address space
    return reserve_discontiguous(reservation);
  };

  const size_t reserved = do_reserve();

  const bool contiguous = reservation->is_contiguous();
  const bool limited = ZAddressSpaceLimit::heap() == ZAddressOffsetMax;

  log_info_p(gc, init)("Address Space Type: %s/%s/%s",
                       (contiguous ? "Contiguous" : "Discontiguous"),
                       (limited ? "Unrestricted" : "Restricted"),
                       (reserved == size ? "Complete" : "Degraded"));
  log_info_p(gc, init)("Address Space Size: %zuM", reserved / M);

  return reserved;
}

bool ZVirtualMemoryManager::is_initialized() const {
  return _initialized;
}

int ZVirtualMemoryManager::shuffle_vmem_to_low_addresses(const ZMemoryRange& vmem, ZArray<ZMemoryRange>* out) {
  const int numa_id = get_numa_id(vmem);
  return _managers.get(numa_id).shuffle_memory_low_addresses(vmem.start(), vmem.size(), out);
}

void ZVirtualMemoryManager::shuffle_vmem_to_low_addresses_contiguous(size_t size, ZArray<ZMemoryRange>* mappings) {
  const int numa_id = get_numa_id(mappings->first());
  _managers.get(numa_id).shuffle_memory_low_addresses_contiguous(size, (ZArray<ZMemoryRange>*)mappings);
}

ZMemoryRange ZVirtualMemoryManager::alloc(size_t size, int numa_id, bool force_low_address) {
  ZMemoryRange range;

  // Small/medium pages are allocated at low addresses, while large pages are
  // allocated at high addresses (unless forced to be at a low address).
  if (force_low_address || size <= ZPageSizeSmall || size <= ZPageSizeMedium) {
    range = _managers.get(numa_id).alloc_low_address(size);
  } else {
    range = _managers.get(numa_id).alloc_high_address(size);
  }

  return range;
}

void ZVirtualMemoryManager::free(const ZMemoryRange& vmem) {
  const int numa_id = get_numa_id(vmem);
  _managers.get(numa_id).free(vmem.start(), vmem.size());
}

int ZVirtualMemoryManager::get_numa_id(const ZMemoryRange& vmem) const {
  for (int numa_id = 0; numa_id < (int)ZNUMA::count(); numa_id++) {
    const ZMemoryRange& range = _vmem_ranges.get(numa_id);
    if (vmem.start() >= range.start() && vmem.end() <= range.end()) {
      return numa_id;
    }
  }

  assert(false, "Should never reach here");
  return -1;
}

zoffset ZVirtualMemoryManager::lowest_available_address(int numa_id) const {
  return _managers.get(numa_id).peek_low_address();
}
