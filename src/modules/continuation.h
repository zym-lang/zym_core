#pragma once

#include "../vm.h"
#include "../object.h"
#include "../value.h"
#include "../zym.h"

bool pushPrompt(VM* vm, ObjPromptTag* tag);
void popPrompt(VM* vm);
PromptEntry* findPrompt(VM* vm, ObjPromptTag* tag);

ObjContinuation* captureContinuation(VM* vm, ObjPromptTag* tag, int return_slot);
bool resumeContinuation(VM* vm, ObjContinuation* cont, Value resume_value);

ZymValue nativeCont_create(ZymVM* vm);
void registerContinuationModule(VM* vm);
