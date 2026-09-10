#pragma once
#include <atomic>
#include <system_error>
#include <thread>

inline std::atomic<bool> fail_allocation{false};
inline std::atomic<std::size_t> allocation_size{0};
inline std::atomic<bool> fail_thread{false};
inline std::atomic<int> allocation_hits{0};
inline std::atomic<int> thread_hits{0};

extern "C" void* __real__Znwm(std::size_t size);

extern "C" void* __wrap__Znwm(std::size_t size) {
  if (fail_allocation.load() &&
      (allocation_size == 0 || allocation_size == size) &&
      fail_allocation.exchange(false)) {
    ++allocation_hits;
    throw std::bad_alloc();
  }
  return __real__Znwm(size);
}

extern "C" void
__real__ZNSt6thread15_M_start_threadESt10unique_ptrINS_6_StateESt14default_deleteIS1_EEPFvvE(
    std::thread*, std::unique_ptr<std::thread::_State>, void (*)());

extern "C" void
__wrap__ZNSt6thread15_M_start_threadESt10unique_ptrINS_6_StateESt14default_deleteIS1_EEPFvvE(
    std::thread* self, std::unique_ptr<std::thread::_State> state,
    void (*entry)()) {
  if (fail_thread.exchange(false)) {
    ++thread_hits;
    throw std::system_error(EAGAIN, std::generic_category(),
                            "thread injection");
  }
  __real__ZNSt6thread15_M_start_threadESt10unique_ptrINS_6_StateESt14default_deleteIS1_EEPFvvE(
      self, std::move(state), entry);
}
