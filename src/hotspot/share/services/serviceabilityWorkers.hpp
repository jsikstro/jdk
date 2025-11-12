/*
 * Copyright (c) 2025, Oracle and/or its affiliates. All rights reserved.
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
 *
 */

#ifndef SHARE_SERVICES_SERVICEABILITYWORKERS_HPP
#define SHARE_SERVICES_SERVICEABILITYWORKERS_HPP

#include "gc/shared/workerThread.hpp"

class ServiceabilityWorkers : public WorkerThreads {
private:
  static ServiceabilityWorkers* _workers;

  ServiceabilityWorkers();

public:
  // Workers are created on-demand, only when they are needed. The number of
  // workers that are created are based on system resources.
  static ServiceabilityWorkers* get_or_create_workers();

  // Get already created workers. The serviceability workers must have been
  // created to call this method.
  static ServiceabilityWorkers* workers();

  // Attempts to set the number of active workers to the requested number of
  // workers. In the case that num_workers exceed the maximum number of workers
  // in the pool, the number of active workers are set to the maximum number
  // of workers in the pool. Use workers()->active_workers() to see the active
  // number of workers.
  static void try_set_requested_workers(uint num_workers);

  // Returns a recommended number of parallel workers to use based on system
  // resources. This is a good default in absence of a user-provided number
  // of workers.
  static uint heuristic_num_parallel_workers();
};

#endif // SHARE_SERVICES_SERVICEABILITYWORKERS_HPP
