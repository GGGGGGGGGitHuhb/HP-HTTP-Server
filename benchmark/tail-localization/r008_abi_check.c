#include <stddef.h>
#include "TailWireV3.h"
_Static_assert(sizeof(HpS4Event) == 32, "event ABI drift");
_Static_assert(sizeof(HpS4Connection) == 64, "connection ABI drift");
_Static_assert(sizeof(HpS4Thread) == 32, "thread ABI drift");
_Static_assert(sizeof(HpS4Failure) == 104, "failure ABI drift");
_Static_assert(sizeof(HpS4Registration) == 32, "registration ABI drift");
_Static_assert(sizeof(HpS4Control) == 25192, "control ABI drift");

_Static_assert(offsetof(HpS4Control, phase) == 16864, "phase offset drift");
_Static_assert(offsetof(HpS4Control, firstFailure) == 16896, "firstFailure offset drift");
_Static_assert(offsetof(HpS4Control, serverRegistrations) == 17000, "registration offset drift");
_Static_assert(offsetof(HpS4Failure, connection) == 32, "failure tuple offset drift");
