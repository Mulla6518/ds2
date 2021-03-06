//
// Copyright (c) 2014, Facebook, Inc.
// All rights reserved.
//
// This source code is licensed under the University of Illinois/NCSA Open
// Source License found in the LICENSE file in the root directory of this
// source tree. An additional grant of patent rights can be found in the
// PATENTS file in the same directory.
//

#ifndef __DebugServer2_Target_Windows_Process_h
#define __DebugServer2_Target_Windows_Process_h

#include "DebugServer2/Host/ProcessSpawner.h"
#include "DebugServer2/Target/ProcessBase.h"

namespace ds2 {
namespace Target {
namespace Windows {

class Process : public Target::ProcessBase {
protected:
  Process();

public:
  virtual ~Process();

public:
  virtual ErrorCode initialize(ProcessId pid, uint32_t flags);

public:
  virtual ErrorCode attach(bool reattach = false) { return kErrorUnsupported; }
  virtual ErrorCode detach() { return kErrorUnsupported; }

public:
  virtual ErrorCode interrupt() { return kErrorUnsupported; }
  virtual ErrorCode terminate() { return kErrorUnsupported; }
  virtual bool isAlive() const { return false; }

public:
  virtual ErrorCode suspend() { return kErrorUnsupported; }
  virtual ErrorCode
  resume(int signal = 0,
         std::set<Thread *> const &excluded = std::set<Thread *>()) {
    return kErrorUnsupported;
  }

public:
  ErrorCode readMemory(Address const &address, void *data, size_t length,
                       size_t *nread = nullptr) {
    return kErrorUnsupported;
  }
  ErrorCode writeMemory(Address const &address, void const *data, size_t length,
                        size_t *nwritten = nullptr) {
    return kErrorUnsupported;
  }

public:
  virtual ErrorCode getMemoryRegionInfo(Address const &address,
                                        MemoryRegionInfo &info) {
    return kErrorUnsupported;
  }

public:
  virtual ErrorCode updateInfo();

public:
  virtual BreakpointManager *breakpointManager() const { return nullptr; }
  virtual WatchpointManager *watchpointManager() const { return nullptr; }

public:
  virtual bool isELFProcess() const { return false; }

public:
  virtual ErrorCode allocateMemory(size_t size, uint32_t protection,
                                   uint64_t *address) {
    return kErrorUnsupported;
  }
  virtual ErrorCode deallocateMemory(uint64_t address, size_t size) {
    return kErrorUnsupported;
  }

public:
  void resetSignalPass() {}
  void setSignalPass(int signo, bool set) {}

public:
  virtual ErrorCode wait(int *status = nullptr, bool hang = true) {
    return kErrorUnsupported;
  }

public:
  static Target::Process *Create(Host::ProcessSpawner &spawner);
  static Target::Process *Attach(ProcessId pid) { return nullptr; }

public:
  virtual ErrorCode getSharedLibraryInfoAddress(Address &address) {
    return kErrorUnsupported;
  }
  virtual ErrorCode enumerateSharedLibraries(
      std::function<void(SharedLibrary const &)> const &cb) {
    return kErrorUnsupported;
  }

public:
  virtual Architecture::GDBDescriptor const *getGDBRegistersDescriptor() const {
    return nullptr;
  }
  virtual Architecture::LLDBDescriptor const *
  getLLDBRegistersDescriptor() const {
    return nullptr;
  }
};
}
}
}

#endif // !__DebugServer2_Target_Windows_Process_h
