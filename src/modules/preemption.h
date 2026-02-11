#pragma once

#include "../vm.h"
#include "../value.h"
#include "../zym.h"

void preemptionEnable(VM* vm);
void preemptionDisable(VM* vm);
bool preemptionIsEnabled(VM* vm);
void preemptionSetTimeslice(VM* vm, int instructions);
int preemptionGetTimeslice(VM* vm);
void preemptionRequest(VM* vm);
void preemptionReset(VM* vm);
int preemptionRemaining(VM* vm);

ZymValue nativePreempt_create(ZymVM* vm);
void registerPreemptionModule(VM* vm);
