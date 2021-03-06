//
// Copyright (c) 2014, Facebook, Inc.
// All rights reserved.
//
// This source code is licensed under the University of Illinois/NCSA Open
// Source License found in the LICENSE file in the root directory of this
// source tree. An additional grant of patent rights can be found in the
// PATENTS file in the same directory.
//

#define __DS2_LOG_CLASS_NAME__ "DebugSession"

#include "DebugServer2/BreakpointManager.h"
#include "DebugServer2/GDBRemote/DebugSessionImpl.h"
#include "DebugServer2/GDBRemote/Session.h"
#include "DebugServer2/Host/Platform.h"
#include "DebugServer2/Utils/HexValues.h"
#include "DebugServer2/Utils/Log.h"

#include <sstream>
#include <iomanip>

using ds2::Host::Platform;
using ds2::Target::Thread;

namespace ds2 {
namespace GDBRemote {

DebugSessionImpl::DebugSessionImpl(StringCollection const &args,
                                   EnvironmentBlock const &env)
    : DummySessionDelegateImpl(), _resumeSession(nullptr) {
  DS2ASSERT(args.size() >= 1);
  _resumeSessionLock.lock();
  spawnProcess(args, env);
}

DebugSessionImpl::DebugSessionImpl(int attachPid)
    : DummySessionDelegateImpl(), _resumeSession(nullptr) {
  _resumeSessionLock.lock();
  _process = ds2::Target::Process::Attach(attachPid);
  if (_process == nullptr)
    DS2LOG(Main, Fatal, "cannot attach to pid %d", attachPid);
}

DebugSessionImpl::DebugSessionImpl()
    : DummySessionDelegateImpl(), _process(nullptr), _resumeSession(nullptr) {
  _resumeSessionLock.lock();
}

DebugSessionImpl::~DebugSessionImpl() {
  _resumeSessionLock.unlock();
  delete _process;
}

size_t DebugSessionImpl::getGPRSize() const {
  if (_process == nullptr)
    return 0;

  ProcessInfo info;
  if (_process->getInfo(info) != kSuccess)
    return 0;

  return info.pointerSize << 3;
}

ErrorCode DebugSessionImpl::onInterrupt(Session &) {
  return _process->interrupt();
}

ErrorCode
DebugSessionImpl::onQuerySupported(Session &session,
                                   Feature::Collection const &remoteFeatures,
                                   Feature::Collection &localFeatures) {
  for (auto feature : remoteFeatures) {
    DS2LOG(DebugSession, Debug, "gdb feature: %s", feature.name.c_str());
  }

  // TODO PacketSize should be respected
  localFeatures.push_back(std::string("PacketSize=3fff"));
  localFeatures.push_back(std::string("ConditionalBreakpoints-"));
  if (_process->breakpointManager() != nullptr) {
    localFeatures.push_back(std::string("BreakpointCommands+"));
  } else {
    localFeatures.push_back(std::string("BreakpointCommands-"));
  }
  localFeatures.push_back(std::string("QPassSignals+"));
  localFeatures.push_back(std::string("QProgramSignals+"));
  localFeatures.push_back(std::string("QStartNoAckMode+"));
  localFeatures.push_back(std::string("QDisableRandomization+"));
  localFeatures.push_back(std::string("QNonStop+"));
  localFeatures.push_back(std::string("multiprocess+"));
  if (_process->isELFProcess()) {
    localFeatures.push_back(std::string("qXfer:auxv:read+"));
  }
  localFeatures.push_back(std::string("qXfer:features:read+"));
  if (_process->isELFProcess()) {
    localFeatures.push_back(std::string("qXfer:libraries-svr4:read+"));
  } else {
    localFeatures.push_back(std::string("qXfer:libraries:read+"));
  }
  localFeatures.push_back(std::string("qXfer:osdata:read+"));
  localFeatures.push_back(std::string("qXfer:siginfo:read+"));
  localFeatures.push_back(std::string("qXfer:siginfo:write+"));
  localFeatures.push_back(std::string("qXfer:threads:read+"));
  // Disable unsupported tracepoints
  localFeatures.push_back(std::string("Qbtrace:bts-"));
  localFeatures.push_back(std::string("Qbtrace:off-"));
  localFeatures.push_back(std::string("tracenz-"));
  localFeatures.push_back(std::string("ConditionalTracepoints-"));
  localFeatures.push_back(std::string("TracepointSource-"));
  localFeatures.push_back(std::string("EnableDisableTracepoints-"));

  return kSuccess;
}

ErrorCode DebugSessionImpl::onPassSignals(Session &session,
                                          std::vector<int> const &signals) {
  _process->resetSignalPass();
  for (int signo : signals) {
    DS2LOG(DebugSession, Debug, "passing signal %d", signo);
    _process->setSignalPass(signo, true);
  }
  return kSuccess;
}

ErrorCode DebugSessionImpl::onProgramSignals(Session &session,
                                             std::vector<int> const &signals) {
  for (int signo : signals) {
    DS2LOG(DebugSession, Debug, "programming signal %d", signo);
    _process->setSignalPass(signo, false);
  }
  return kSuccess;
}

ErrorCode DebugSessionImpl::onNonStopMode(Session &session, bool enable) {
  if (enable)
    return kErrorUnsupported; // TODO support non-stop mode

  return kSuccess;
}

Thread *DebugSessionImpl::findThread(ProcessThreadId const &ptid) const {
  if (_process == nullptr)
    return nullptr;

  if (ptid.pid > 0 && ptid.pid != _process->pid())
    return nullptr;

  Thread *thread = nullptr;
  if (ptid.tid <= 0) {
    thread = _process->currentThread();
  } else {
    thread = _process->thread(ptid.tid);
  }

  return thread;
}

ErrorCode DebugSessionImpl::queryStopCode(Session &session,
                                          ProcessThreadId const &ptid,
                                          StopCode &stop) const {
  Thread *thread = findThread(ptid);
  DS2LOG(DebugSession, Debug, "thread %p", thread);
  if (thread == nullptr)
    return kErrorProcessNotFound;

  bool readRegisters = true;
  TrapInfo const &trap = thread->trapInfo();

  Architecture::CPUState state;

  stop.ptid.pid = trap.pid;
  stop.ptid.tid = trap.tid;
  stop.core = trap.core;
  stop.reason = StopCode::kSignalStop;
  switch (trap.event) {
  case TrapInfo::kEventNone:
    stop.reason = StopCode::kNone;
    break;
  case TrapInfo::kEventExit:
    stop.event = StopCode::kCleanExit;
    stop.status = trap.status;
    readRegisters = false;
    break;
  case TrapInfo::kEventKill:
  case TrapInfo::kEventCoreDump:
    stop.event = StopCode::kSignalExit;
    stop.signal = trap.signal;
    readRegisters = false;
    break;
  case TrapInfo::kEventTrap:
    stop.event = StopCode::kSignal;
    stop.reason = StopCode::kBreakpoint;
    stop.signal = trap.signal;
    break;
  case TrapInfo::kEventStop:
    stop.event = StopCode::kSignal;
    stop.reason = StopCode::kSignalStop;
    stop.signal = trap.signal;
    break;
  }

  if (readRegisters) {
    stop.threadName = Platform::GetThreadName(stop.ptid.pid, stop.ptid.tid);
    ErrorCode error = thread->readCPUState(state);
    if (error != kSuccess)
      return error;
    state.getStopGPState(stop.registers,
                         session.mode() == kCompatibilityModeLLDB);
  }

  _process->enumerateThreads(
      [&](Thread *thread) { stop.threads.insert(thread->tid()); });

  return kSuccess;
}

ErrorCode DebugSessionImpl::onQueryThreadStopInfo(Session &session,
                                                  ProcessThreadId const &ptid,
                                                  bool list, StopCode &stop) {
  Thread *thread = findThread(ptid);
  if (thread == nullptr)
    return kErrorProcessNotFound;

  return queryStopCode(session, ptid, stop);
}

ErrorCode DebugSessionImpl::onQueryThreadList(Session &, ProcessId pid,
                                              ThreadId lastTid, ThreadId &tid) {
  if (_process == nullptr)
    return kErrorProcessNotFound;

  switch (lastTid) {
  case kAllThreadId:
    _threadIndex = 0;
    _process->getThreadIds(_tids);
    break;

  case kAnyThreadId:
    _threadIndex++;
    break;

  default:
    return kErrorInvalidArgument;
  }

  if (_threadIndex >= _tids.size())
    return kErrorNotFound;

  tid = _tids[_threadIndex];
  return kSuccess;
}

ErrorCode DebugSessionImpl::onQueryCurrentThread(Session &,
                                                 ProcessThreadId &ptid) {
  if (_process == nullptr)
    return kErrorProcessNotFound;

  Thread *thread = _process->currentThread();
  if (thread == nullptr)
    return kErrorProcessNotFound;

  ptid.pid = _process->pid();
  ptid.tid = thread->tid();
  return kSuccess;
}

ErrorCode DebugSessionImpl::onThreadIsAlive(Session &session,
                                            ProcessThreadId const &ptid) {
  if (_process == nullptr)
    return kErrorProcessNotFound;

  Thread *thread = findThread(ptid);
  if (thread == nullptr)
    return kErrorProcessNotFound;

  if (thread->state() == Thread::kTerminated)
    return kErrorInvalidArgument;

  return kSuccess;
}

ErrorCode DebugSessionImpl::onQueryAttached(Session &, ProcessId pid,
                                            bool &attachedProcess) {
  if (_process == nullptr)
    return kErrorProcessNotFound;
  if (pid > 0 && pid != _process->pid())
    return kErrorProcessNotFound;

  attachedProcess = _process->attached();
  return kSuccess;
}

ErrorCode DebugSessionImpl::onQueryProcessInfo(Session &, ProcessInfo &info) {
  if (_process == nullptr)
    return kErrorProcessNotFound;
  else
    return _process->getInfo(info);
}

ErrorCode DebugSessionImpl::onQueryRegisterInfo(Session &, uint32_t regno,
                                                RegisterInfo &info) {
  Architecture::LLDBRegisterInfo reginfo;
  Architecture::LLDBDescriptor const *desc =
      _process->getLLDBRegistersDescriptor();

  if (!Architecture::LLDBGetRegisterInfo(*desc, regno, reginfo))
    return kErrorInvalidArgument;

  if (reginfo.SetName != nullptr) {
    info.setName = reginfo.SetName;
  }

  if (reginfo.Def->LLDBName != nullptr) {
    info.registerName = reginfo.Def->LLDBName;
  } else {
    info.registerName = reginfo.Def->Name;
  }

  if (reginfo.Def->AlternateName != nullptr) {
    info.alternateName = reginfo.Def->AlternateName;
  }
  if (reginfo.Def->GenericName != nullptr) {
    info.genericName = reginfo.Def->GenericName;
  }

  info.bitSize = reginfo.Def->BitSize;
  info.byteOffset = reginfo.Def->LLDBOffset;
  info.gccRegisterIndex = reginfo.Def->GCCRegisterNumber;
  info.dwarfRegisterIndex = reginfo.Def->DWARFRegisterNumber;

  if (reginfo.Def->Format == Architecture::kFormatVector) {
    info.encoding = RegisterInfo::kEncodingVector;
    switch (reginfo.Def->LLDBVectorFormat) {
    case Architecture::kLLDBVectorFormatUInt8:
      info.format = RegisterInfo::kFormatVectorUInt8;
      break;
    case Architecture::kLLDBVectorFormatSInt8:
      info.format = RegisterInfo::kFormatVectorSInt8;
      break;
    case Architecture::kLLDBVectorFormatUInt16:
      info.format = RegisterInfo::kFormatVectorUInt16;
      break;
    case Architecture::kLLDBVectorFormatSInt16:
      info.format = RegisterInfo::kFormatVectorSInt16;
      break;
    case Architecture::kLLDBVectorFormatUInt32:
      info.format = RegisterInfo::kFormatVectorUInt32;
      break;
    case Architecture::kLLDBVectorFormatSInt32:
      info.format = RegisterInfo::kFormatVectorSInt32;
      break;
    case Architecture::kLLDBVectorFormatUInt128:
      info.format = RegisterInfo::kFormatVectorUInt128;
      break;
    case Architecture::kLLDBVectorFormatFloat32:
      info.format = RegisterInfo::kFormatVectorFloat32;
      break;
    default:
      info.format = RegisterInfo::kFormatVectorUInt8;
      break;
    }
  } else if (reginfo.Def->Format == Architecture::kFormatFloat) {
    info.encoding = RegisterInfo::kEncodingIEEE754;
    info.format = RegisterInfo::kFormatFloat;
  } else {
    switch (reginfo.Def->Encoding) {
    case Architecture::kEncodingUInteger:
      info.encoding = RegisterInfo::kEncodingUInt;
      break;
    case Architecture::kEncodingSInteger:
      info.encoding = RegisterInfo::kEncodingSInt;
      break;
    case Architecture::kEncodingIEEESingle:
    case Architecture::kEncodingIEEEDouble:
    case Architecture::kEncodingIEEEExtended:
      info.encoding = RegisterInfo::kEncodingIEEE754;
      break;
    default:
      info.encoding = RegisterInfo::kEncodingUInt;
      break;
    }

    switch (reginfo.Def->Format) {
    case Architecture::kFormatBinary:
      info.format = RegisterInfo::kFormatBinary;
      break;
    case Architecture::kFormatDecimal:
      info.format = RegisterInfo::kFormatDecimal;
      break;
    case Architecture::kFormatHexadecimal:
    default:
      info.format = RegisterInfo::kFormatHex;
      break;
    }
  }

  if (reginfo.Def->ContainerRegisters != nullptr) {
    for (size_t n = 0; reginfo.Def->ContainerRegisters[n] != nullptr; n++) {
      info.containerRegisters.push_back(
          reginfo.Def->ContainerRegisters[n]->LLDBRegisterNumber);
    }
  }

  if (reginfo.Def->InvalidatedRegisters != nullptr) {
    for (size_t n = 0; reginfo.Def->InvalidatedRegisters[n] != nullptr; n++) {
      info.invalidateRegisters.push_back(
          reginfo.Def->InvalidatedRegisters[n]->LLDBRegisterNumber);
    }
  }

  return kSuccess;
}

ErrorCode
DebugSessionImpl::onQuerySharedLibrariesInfoAddress(Session &,
                                                    Address &address) {
  if (_process == nullptr)
    return kErrorProcessNotFound;

  return _process->getSharedLibraryInfoAddress(address);
}

ErrorCode DebugSessionImpl::onXferRead(Session &, std::string const &object,
                                       std::string const &annex,
                                       uint64_t offset, uint64_t length,
                                       std::string &buffer, bool &last) {
  DS2LOG(DebugSession, Info, "object='%s' annex='%s' offset=%#llx length=%#llx",
         object.c_str(), annex.c_str(), (unsigned long long)offset,
         (unsigned long long)length);

  // TODO Split these generators into appropriate functions
  if (object == "features") {
    Architecture::GDBDescriptor const *desc =
        _process->getGDBRegistersDescriptor();
    if (annex == "target.xml") {
      buffer = Architecture::GDBGenerateXMLMain(*desc).substr(offset);
    } else {
      buffer = Architecture::GDBGenerateXMLFeatureByFileName(*desc, annex)
                   .substr(offset);
    }
    if (buffer.length() > length) {
      buffer.resize(length);
      last = false;
    }
    return kSuccess;
  } else if (object == "auxv") {
    ErrorCode error = _process->getAuxiliaryVector(buffer);
    if (error != kSuccess)
      return error;

    buffer = buffer.substr(offset);
    if (buffer.length() > length) {
      buffer.resize(length);
      last = false;
    }

    return kSuccess;
  } else if (object == "threads") {
    std::ostringstream ss;

    ss << "<threads>" << std::endl;

    _process->enumerateThreads([&](Thread *thread) {
      ss << "<thread "
         << "id=\"p" << std::hex << _process->pid() << '.' << std::hex
         << thread->tid() << "\" "
         << "core=\"" << std::dec << thread->core() << "\""
         << "/>" << std::endl;
    });

    ss << "</threads>" << std::endl;

    buffer = ss.str().substr(offset);
    if (buffer.length() > length) {
      buffer.resize(length);
      last = false;
    }
    return kSuccess;
  } else if (object == "libraries-svr4") {
    std::ostringstream ss;
    std::ostringstream sslibs;
    Address mainMapAddress;

    if (_process->isELFProcess()) {
      _process->enumerateSharedLibraries(
          [&](Target::Process::SharedLibrary const &library) {
            if (library.main) {
              mainMapAddress = library.svr4.mapAddress;
            } else {
              sslibs << "<library "
                     << "name=\"" << library.path << "\" "
                     << "lm=\""
                     << "0x" << std::hex << library.svr4.mapAddress << "\" "
                     << "l_addr=\""
                     << "0x" << std::hex << library.svr4.baseAddress << "\" "
                     << "l_ld=\""
                     << "0x" << std::hex << library.svr4.ldAddress << "\" "
                     << "/>" << std::endl;
            }
          });

      ss << "<library-list-svr4 version=\"1.0\"";
      if (mainMapAddress.valid()) {
        ss << " main-lm=\""
           << "0x" << std::hex << mainMapAddress.value() << "\"";
      }
      ss << ">" << std::endl;
      ss << sslibs.str();
      ss << "</library-list-svr4>";
      buffer = ss.str().substr(offset);
      if (buffer.length() > length) {
        buffer.resize(length);
        last = false;
      }
      return kSuccess;
    }
  }

  return kErrorUnsupported;
}

ErrorCode DebugSessionImpl::onReadGeneralRegisters(
    Session &, ProcessThreadId const &ptid,
    Architecture::GPRegisterValueVector &regs) {
  Thread *thread = findThread(ptid);
  if (thread == nullptr)
    return kErrorProcessNotFound;

  Architecture::CPUState state;
  ErrorCode error = thread->readCPUState(state);
  if (error != kSuccess)
    return error;

  state.getGPState(regs);

  return kSuccess;
}

ErrorCode DebugSessionImpl::onWriteGeneralRegisters(
    Session &, ProcessThreadId const &ptid, std::vector<uint64_t> const &regs) {
  Thread *thread = findThread(ptid);
  if (thread == nullptr)
    return kErrorProcessNotFound;

  Architecture::CPUState state;
  ErrorCode error = thread->readCPUState(state);
  if (error != kSuccess)
    return error;

  state.setGPState(regs);

  return thread->writeCPUState(state);
}

ErrorCode DebugSessionImpl::onSaveRegisters(Session &session,
                                            ProcessThreadId const &ptid,
                                            uint64_t &id) {
  static uint64_t counter = 1;

  Thread *thread = findThread(ptid);
  if (thread == nullptr)
    return kErrorProcessNotFound;

  Architecture::CPUState state;
  ErrorCode error = thread->readCPUState(state);
  if (error != kSuccess)
    return error;

  _savedRegisters[counter] = state;
  id = counter++;
  return kSuccess;
}

ErrorCode DebugSessionImpl::onRestoreRegisters(Session &session,
                                               ProcessThreadId const &ptid,
                                               uint64_t id) {
  Thread *thread = findThread(ptid);
  if (thread == nullptr)
    return kErrorProcessNotFound;

  auto it = _savedRegisters.find(id);
  if (it == _savedRegisters.end())
    return kErrorNotFound;

  ErrorCode error = thread->writeCPUState(it->second);
  if (error != kSuccess)
    return error;

  _savedRegisters.erase(it);

  return kSuccess;
}

ErrorCode DebugSessionImpl::onReadRegisterValue(Session &session,
                                                ProcessThreadId const &ptid,
                                                uint32_t regno,
                                                std::string &value) {
  Thread *thread = findThread(ptid);
  if (thread == nullptr)
    return kErrorProcessNotFound;

  Architecture::CPUState state;
  ErrorCode error = thread->readCPUState(state);
  if (error != kSuccess)
    return error;

  void *ptr;
  size_t length;
  bool success;

  if (session.mode() == kCompatibilityModeLLDB) {
    success = state.getLLDBRegisterPtr(regno, &ptr, &length);
  } else {
    success = state.getGDBRegisterPtr(regno, &ptr, &length);
  }

  if (!success)
    return kErrorInvalidArgument;

  value.insert(value.end(), reinterpret_cast<char *>(ptr),
               reinterpret_cast<char *>(ptr) + length);

  return kSuccess;
}

ErrorCode DebugSessionImpl::onWriteRegisterValue(Session &session,
                                                 ProcessThreadId const &ptid,
                                                 uint32_t regno,
                                                 std::string const &value) {
  Thread *thread = findThread(ptid);
  if (thread == nullptr)
    return kErrorProcessNotFound;

  Architecture::CPUState state;
  ErrorCode error = thread->readCPUState(state);
  if (error != kSuccess)
    return error;

  void *ptr;
  size_t length;
  bool success;

  if (session.mode() == kCompatibilityModeLLDB) {
    success = state.getLLDBRegisterPtr(regno, &ptr, &length);
  } else {
    success = state.getGDBRegisterPtr(regno, &ptr, &length);
  }

  if (!success)
    return kErrorInvalidArgument;

  if (value.length() != length)
    return kErrorInvalidArgument;

  std::memcpy(ptr, value.c_str(), length);

  return thread->writeCPUState(state);
}

ErrorCode DebugSessionImpl::onReadMemory(Session &, Address const &address,
                                         size_t length, std::string &data) {
  if (_process == nullptr)
    return kErrorProcessNotFound;
  else
    return _process->readMemoryBuffer(address, length, data);
}

ErrorCode DebugSessionImpl::onWriteMemory(Session &, Address const &address,
                                          std::string const &data,
                                          size_t &nwritten) {
  if (_process == nullptr)
    return kErrorProcessNotFound;
  else
    return _process->writeMemoryBuffer(address, data, &nwritten);
}

ErrorCode DebugSessionImpl::onAllocateMemory(Session &, size_t size,
                                             uint32_t permissions,
                                             Address &address) {
  uint64_t addr;
  ErrorCode error = _process->allocateMemory(size, permissions, &addr);
  if (error == kSuccess) {
    _allocations[addr] = size;
    address = addr;
  }
  return error;
}

ErrorCode DebugSessionImpl::onDeallocateMemory(Session &,
                                               Address const &address) {
  auto i = _allocations.find(address);
  if (i == _allocations.end())
    return kErrorInvalidArgument;

  ErrorCode error = _process->deallocateMemory(address, i->second);
  if (error != kSuccess)
    return error;

  _allocations.erase(i);
  return kSuccess;
}

ErrorCode
DebugSessionImpl::onSetProgramArguments(Session &,
                                        StringCollection const &args) {
  spawnProcess(args, {});
  if (_process == nullptr)
    return kErrorUnknown;

  return kSuccess;
}

ErrorCode DebugSessionImpl::onQueryLaunchSuccess(Session &, ProcessId) {
  return kSuccess;
}

ErrorCode DebugSessionImpl::onAttach(Session &session, ProcessId pid,
                                     AttachMode mode, StopCode &stop) {
  if (_process != nullptr)
    return kErrorAlreadyExist;

  if (mode != kAttachNow)
    return kErrorInvalidArgument;

  DS2LOG(SlaveSession, Info, "attaching to pid %u", pid);
  _process = Target::Process::Attach(pid);
  DS2LOG(SlaveSession, Debug, "_process=%p", _process);
  if (_process == nullptr)
    return kErrorProcessNotFound;

  return queryStopCode(session, pid, stop);
}

ErrorCode
DebugSessionImpl::onResume(Session &session,
                           ThreadResumeAction::Collection const &actions,
                           StopCode &stop) {
  ErrorCode error;
  ThreadResumeAction globalAction;
  bool hasGlobalAction = false;
  std::set<Thread *> excluded;

  DS2ASSERT(_resumeSession == nullptr);
  _resumeSession = &session;
  _resumeSessionLock.unlock();

  error = _process->beforeResume();
  if (error != kSuccess)
    goto ret;

  //
  // First process all actions that specify a thread,
  // save the global and trigger it later.
  //
  for (auto const &action : actions) {
    if (action.ptid.any()) {
      if (hasGlobalAction) {
        DS2LOG(DebugSession, Error, "more than one global action specified");
        error = kErrorAlreadyExist;
        goto ret;
      }

      globalAction = action;
      hasGlobalAction = true;
      continue;
    }

    Thread *thread = findThread(action.ptid);
    if (thread == nullptr) {
      DS2LOG(DebugSession, Warning, "pid %d tid %d not found", action.ptid.pid,
             action.ptid.tid);
      continue;
    }

    if (action.action == kResumeActionContinue ||
        action.action == kResumeActionContinueWithSignal) {
      error = thread->resume(action.signal, action.address);
      if (error != kSuccess) {
        DS2LOG(DebugSession, Warning, "cannot resume pid %d tid %d, error=%d",
               _process->pid(), thread->tid(), error);
        continue;
      }
      excluded.insert(thread);
    } else if (action.action == kResumeActionSingleStep ||
               action.action == kResumeActionSingleStepWithSignal) {
      error = thread->step(action.signal, action.address);
      if (error != kSuccess) {
        DS2LOG(DebugSession, Warning, "cannot resume pid %d tid %d, error=%d",
               _process->pid(), thread->tid(), error);
        continue;
      }
      excluded.insert(thread);
    } else {
      DS2LOG(DebugSession, Warning,
             "cannot resume pid %d tid %d, action %d not yet implemented",
             _process->pid(), thread->tid(), action.action);
      continue;
    }
  }

  //
  // Now trigger the global action
  //
  if (hasGlobalAction) {
    if (globalAction.action == kResumeActionContinue ||
        globalAction.action == kResumeActionContinueWithSignal) {
      if (globalAction.address.valid()) {
        DS2LOG(DebugSession, Warning, "global continue with address");
      }

      error = _process->resume(globalAction.signal, excluded);
      if (error != kSuccess && error != kErrorAlreadyExist) {
        DS2LOG(DebugSession, Warning, "cannot resume pid %d, error=%d",
               _process->pid(), error);
      }
    } else if (globalAction.action == kResumeActionSingleStep ||
               globalAction.action == kResumeActionSingleStepWithSignal) {
      Thread *thread = _process->currentThread();
      if (excluded.find(thread) == excluded.end()) {
        error = thread->step(globalAction.signal, globalAction.address);
        if (error != kSuccess) {
          DS2LOG(DebugSession, Warning, "cannot resume pid %d tid %d, error=%d",
                 _process->pid(), thread->tid(), error);
        }
      }
    } else {
      DS2LOG(DebugSession, Warning,
             "cannot resume pid %d, action %d not yet implemented",
             _process->pid(), globalAction.action);
    }
  }

  //
  // If kErrorAlreadyExist is set, then a signal is already pending.
  //
  if (error != kErrorAlreadyExist) {
    //
    // Wait for the next signal
    //
    error = _process->wait();
    if (error != kSuccess)
      goto ret;
  }

  error = _process->afterResume();
  if (error != kSuccess)
    goto ret;

  error = queryStopCode(
      session,
      ProcessThreadId(_process->pid(), _process->currentThread()->tid()), stop);

ret:
  _resumeSessionLock.lock();
  _resumeSession = nullptr;
  return error;
}

ErrorCode DebugSessionImpl::onDetach(Session &, ProcessId, bool stopped) {
  ErrorCode error;

  BreakpointManager *bpm = _process->breakpointManager();
  if (bpm != nullptr) {
    bpm->clear();
  }

  if (stopped) {
    error = _process->suspend();
    if (error != kSuccess)
      return error;
  }

  return _process->detach();
}

ErrorCode DebugSessionImpl::onTerminate(Session &session,
                                        ProcessThreadId const &ptid,
                                        StopCode &stop) {
  ErrorCode error;

  error = _process->terminate();
  if (error != kSuccess) {
    DS2LOG(DebugSession, Error, "couldn't terminate process");
    return error;
  }

  error = _process->wait();
  if (error != kSuccess) {
    DS2LOG(DebugSession, Error, "couldn't wait for process termination");
    return error;
  }

  return queryStopCode(session, _process->pid(), stop);
}

//
// For LLDB we need to support breakpoints through the breakpoint manager
// because LLDB is unable to handle software breakpoints.
// In GDB mode we let GDB handle the breakpoints.
//
ErrorCode DebugSessionImpl::onInsertBreakpoint(
    Session &session, BreakpointType type, Address const &address,
    uint32_t size, StringCollection const &, StringCollection const &, bool) {
  //    if (session.mode() != kCompatibilityModeLLDB)
  //        return kErrorUnsupported;

  if (type != kSoftwareBreakpoint)
    return kErrorUnsupported;

  BreakpointManager *bpm = _process->breakpointManager();
  if (bpm == nullptr)
    return kErrorUnsupported;

  return bpm->add(address, BreakpointManager::kTypePermanent, size);
}

ErrorCode DebugSessionImpl::onRemoveBreakpoint(Session &session,
                                               BreakpointType type,
                                               Address const &address,
                                               uint32_t size) {
  //    if (session.mode() != kCompatibilityModeLLDB)
  //        return kErrorUnsupported;

  if (type != kSoftwareBreakpoint)
    return kErrorUnsupported;

  BreakpointManager *bpm = _process->breakpointManager();
  if (bpm == nullptr)
    return kErrorUnsupported;

  return bpm->remove(address);
}

ErrorCode DebugSessionImpl::spawnProcess(StringCollection const &args,
                                         EnvironmentBlock const &env) {
  DS2LOG(DebugSession, Debug, "spawning process with args:");
  for (auto const &arg : args)
    DS2LOG(DebugSession, Debug, "  %s", arg.c_str());
  DS2LOG(DebugSession, Debug, "and with environment:");
  for (auto const &val : env)
    DS2LOG(DebugSession, Debug, "  %s=%s", val.first.c_str(),
           val.second.c_str());

  _spawner.setExecutable(args[0]);
  _spawner.setArguments(StringCollection(args.begin() + 1, args.end()));
  _spawner.setEnvironment(env);

  auto outputDelegate = [this](void *buf, size_t size) {
    const char *cbuf = static_cast<char *>(buf);
    for (size_t i = 0; i < size; ++i) {
      this->_consoleBuffer += cbuf[i];
      if (cbuf[i] == '\n') {
        _resumeSessionLock.lock();
        DS2ASSERT(_resumeSession != nullptr);
        std::string data = "O";
        data += StringToHex(this->_consoleBuffer);
        _consoleBuffer.clear();
        this->_resumeSession->send(data);
        _resumeSessionLock.unlock();
      }
    }
  };

  _spawner.redirectOutputToDelegate(outputDelegate);
  _spawner.redirectErrorToDelegate(outputDelegate);

  _process = ds2::Target::Process::Create(_spawner);
  if (_process == nullptr) {
    DS2LOG(Main, Error, "cannot execute '%s'", args[0].c_str());
    return kErrorUnknown;
  }

  return kSuccess;
}
}
}
