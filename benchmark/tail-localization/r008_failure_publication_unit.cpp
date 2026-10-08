#include "TailWire.h"
#include <atomic>
#include <cassert>
#include <thread>

std::atomic<bool> winnerCapturedSlot{false};
std::atomic<bool> releaseWinner{false};

bool captureSlotAndPause(unsigned* target, unsigned* expected, unsigned desired,
                         bool weak, int successOrder, int failureOrder) {
  const auto claimed = __atomic_compare_exchange_n(target, expected, desired, weak,
                                                   successOrder, failureOrder);
  if (claimed) {
    winnerCapturedSlot.store(true, std::memory_order_release);
    while (!releaseWinner.load(std::memory_order_acquire)) std::this_thread::yield();
  }
  return claimed;
}

// 仅测试编译单元在真实CAS成功后暂停winner，不改变产品原子发布协议。
#define __atomic_compare_exchange_n captureSlotAndPause
#include "StartupEvidence.h"
#undef __atomic_compare_exchange_n

void publishWinningFailure(HpS4Control& control) {
  const auto triggerPhase = __atomic_load_n(&control.phase, __ATOMIC_ACQUIRE);
  HpS4Connection connection{};
  connection.worker = 2;
  connection.fd = 12;
  publishStartupFailure(&control, 0, HP_S4_WORKER_RANGE, &connection, 102,
                        triggerPhase);
}

int main() {
  HpS4Control control{};
  control.phase = HP_S4_REGISTERING;
  std::thread winner(publishWinningFailure, std::ref(control));
  while (!winnerCapturedSlot.load(std::memory_order_acquire)) std::this_thread::yield();
  assert(__atomic_load_n(&control.firstFailure.published, __ATOMIC_ACQUIRE) == 1);
  HpS4Connection cleanupConnection{};
  const auto secondaryPhase = __atomic_load_n(&control.phase, __ATOMIC_ACQUIRE);
  publishStartupFailure(&control, 1, HP_S4_CLOSED_BEFORE_GO, &cleanupConnection,
                        456, secondaryPhase);
  assert(__atomic_load_n(&control.phase, __ATOMIC_ACQUIRE) == HP_S4_ABORTED);
  assert(__atomic_load_n(&control.firstFailure.published, __ATOMIC_ACQUIRE) == 1);
  releaseWinner.store(true, std::memory_order_release);
  winner.join();
  assert(__atomic_load_n(&control.firstFailure.published, __ATOMIC_ACQUIRE) == 2);
  const auto& failure = control.firstFailure;
  assert(failure.phase == HP_S4_REGISTERING);
  assert(failure.code == HP_S4_WORKER_RANGE);
  assert(failure.worker == 2 && failure.connection.fd == 12);
  assert(failure.savedErrno == 102);
  return 0;
}
