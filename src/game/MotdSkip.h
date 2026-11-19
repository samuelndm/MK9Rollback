#pragma once
// MK9's "message of the day" ("Verificando mensagens do dia") without the wait for its dead server.
//
// On the first online entry of a game start, MK9's script MKOnline (Multiplayer_GetMOTD) asks the engine for the
// message of the day and keeps that screen up, one frame at a time, until the answer comes. The engine (0x0093F790)
// asks https://mk.hydra.agoragames.com, Agora Games' service for MK9, which no longer exists: the request ends only
// when the name's lookup fails (about 3 s through the user's router in Nov 2026; 10 s is MK9's cap), and the screen then
// says the message is unavailable. MK9 has its own switch for this, DisableAgoraMOTD (online config 0x00E92540 +0x24,
// or DisableAgoraMaster +0x10): when it is on, the engine calls the screen's callback at once with no answer, and the
// script shows the same "unavailable" text with no network at all.
//
// The switch is read through one function, 0x0081E3C0 ("MOTD off?"); its callers are the engine's GetMOTD and a script
// native (0x008E3E50) that no script calls. Making it answer 1 turns MK9's switch on. Only this PC's menu changes:
// nothing of it reaches the other player or a match.

#include <cstdint>

#include "../debug/Logger.h"
#include "../hook/CodePatch.h"

namespace MotdSkip {

constexpr uintptr_t kMotdOff = 0x0081E3C0;

inline bool Install() {
    // call 0x0081E200 (the online config); cmp dword [eax+0x24], 0  ->  mov eax, 1; ret
    static const uint8_t kOrig[6] = {0xE8, 0x3B, 0xFE, 0xFF, 0xFF, 0x83};
    static const uint8_t kOn[6] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3};
    const bool ok = CodePatch::Write(kMotdOff, kOrig, kOn, 6,
                                     "MOTD off? 0x0081E3C0 (message of the day: no wait for MK9's dead Agora server)");
    if (!ok) Logger::Info("MOTDSKIP: not installed; MK9 asks its dead server for the message of the day as before");
    return ok;
}

}  // namespace MotdSkip
