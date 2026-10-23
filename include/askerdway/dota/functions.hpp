// Generated known ABI bindings; the actual engine implementation is not included.
#pragma once
#include "askerdway/dota_sdk.hpp"
namespace askerdway::dota::functions {
/// Submits a Dota unit order through the local player controller. Must run on the engine-required thread.
inline function<void(void*, int, int, const askerdway_vec3*, int, int, void*, int, bool)> bind_C_DOTAPlayerController_PrepareUnitOrders() {
    return bind_signature<void(void*, int, int, const askerdway_vec3*, int, int, void*, int, bool)>("client.dll", "C_DOTAPlayerController::PrepareUnitOrders", "void(void*, int, int, const askerdway_vec3*, int, int, void*, int, bool)");
}
inline void C_DOTAPlayerController_PrepareUnitOrders(void* arg0, int arg1, int arg2, const askerdway_vec3* arg3, int arg4, int arg5, void* arg6, int arg7, bool arg8) {
    static const auto native = bind_C_DOTAPlayerController_PrepareUnitOrders();
    return native(arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8);
}
/// Returns the player-controller pointer for a local split-screen slot. Must run on the engine-required thread.
inline function<void*(int)> bind_GetLocalPlayerController() {
    return bind_signature<void*(int)>("client.dll", "GetLocalPlayerController", "void*(int)");
}
inline void* GetLocalPlayerController(int arg0) {
    static const auto native = bind_GetLocalPlayerController();
    return native(arg0);
}
/// Advances one client particle effect by a frame delta. Must run on the engine-required thread.
inline function<void(void*, float)> bind_CNewParticleEffect_Update() {
    return bind_signature<void(void*, float)>("client.dll", "CNewParticleEffect::Update", "void(void*, float)");
}
inline void CNewParticleEffect_Update(void* arg0, float arg1) {
    static const auto native = bind_CNewParticleEffect_Update();
    return native(arg0, arg1);
}
}
