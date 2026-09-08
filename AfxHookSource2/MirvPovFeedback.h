#pragma once

#include <Windows.h>

namespace SOURCESDK { namespace CS2 { class IGameEvent; } }

void MirvPovFeedback_Initialize(HMODULE clientDll);
void MirvPovFeedback_HandleGameEvent(SOURCESDK::CS2::IGameEvent * event);
