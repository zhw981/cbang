/******************************************************************************\

          This file is part of the C! library.  A.K.A the cbang library.

                Copyright (c) 2003-2019, Cauldron Development LLC
                   Copyright (c) 2003-2017, Stanford University
                               All rights reserved.

         The C! library is free software: you can redistribute it and/or
        modify it under the terms of the GNU Lesser General Public License
       as published by the Free Software Foundation, either version 2.1 of
               the License, or (at your option) any later version.

        The C! library is distributed in the hope that it will be useful,
          but WITHOUT ANY WARRANTY; without even the implied warranty of
        MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
                 Lesser General Public License for more details.

         You should have received a copy of the GNU Lesser General Public
                 License along with the C! library.  If not, see
                         <http://www.gnu.org/licenses/>.

        In addition, BSD licensing may be granted on a case by case basis
        by written permission from at least one of the copyright holders.
           You may request written permission by emailing the authors.

                  For information regarding this software email:
                                 Joseph Coffland
                          joseph@cauldrondevelopment.com

\******************************************************************************/

#include "ConcurrentPool.h"

#include <cbang/Catch.h>

using namespace cb::Event;
using namespace cb;
using namespace std;


bool ConcurrentPool::Task::shouldShutdown() {
  return Thread::current().shouldShutdown();
}


ConcurrentPool::ConcurrentPool(cb::Event::Base &base, unsigned size) :
  ThreadPool(size), base(base),
  event(base.newEvent(this, &ConcurrentPool::complete, EF::EVENT_NO_SELF_REF)) {
  if (!Base::threadsEnabled())
    THROW("Cannot use Event::ConcurrentPool without threads enabled.  "
          "Call Event::Base::enableThreads() before creating Event::Base.");
}


ConcurrentPool::~ConcurrentPool() {}


unsigned ConcurrentPool::getNumReady() const {
  SmartLock lock(this);
  return ready.size();
}


unsigned ConcurrentPool::getNumCompleted() const {
  SmartLock lock(this);
  return completed.size();
}


void ConcurrentPool::submit(const SmartPointer<Task> &task) {
  SmartLock lock(this);
  ready.push(task);
  Condition::signal();
}


void ConcurrentPool::stop() {
  ThreadPool::stop();
  Condition::broadcast();
}


void ConcurrentPool::join() {
  stop();
  ThreadPool::wait();
}


void ConcurrentPool::run() {
  SmartLock lock(this);

  while (!Thread::current().shouldShutdown()) {
    Condition::wait();
    if (Thread::current().shouldShutdown()) break;

    // Get Task from ready queue
    if (ready.empty()) continue;
    SmartPointer<Task> task = ready.top();
    ready.pop();

    {
      SmartUnlock unlock(this);

      // Run Task
      try {
        task->run();

      } catch (const Exception &e) {
        task->setException(e);

      } catch (const std::exception &e) {
        task->setException(string(e.what()));

      } catch (...) {
        task->setException(string("Unknown exception"));
      }
    }

    // Put Task in completed queue
    completed.push(task);
    event->activate();
  }
}


void ConcurrentPool::complete() {
  SmartLock lock(this);

  // Dequeue completed tasks
  while (!completed.empty()) {
    SmartPointer<Task> task = completed.top();
    completed.pop();

    SmartUnlock unlock(this);

    try {
      if (task->getFailed()) task->error(task->getException());
      else task->success();
    } CATCH_ERROR;

    try {task->complete();} CATCH_ERROR;
  }
}
