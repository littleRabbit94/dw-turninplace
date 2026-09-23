// DWTurnInPlace, phase 0 spike: proves the input hook the turn-in-place mod needs. Logs only, except F7.
//
// APawn::AddMovementInput is virtual: its exec thunk ends in `call qword ptr [rax+858h]` (slot 267 on the
// 2026-09-10 build). The slot is read out of the thunk at load, cross-checked against 267, and hooked on
// DawnwalkerPlayerCharacter's vtable (every BP_PlayerCharacter_C shares it), the way DWSmoothwalker hooks
// GetCameraView.
//
// Questions this build answers, one log line each:
//   1. Does the game's input path call the virtual at all? ("moving without hook" counts frames where the
//      movement component has acceleration but the hook never saw input.)
//   2. Does it run before the movement component's tick in the same frame? (START line: acceleration read
//      inside the hook versus at the end of the frame. Zero, then non-zero means before.)
//   3. Does a pop from inside the hook land in that frame? (F7 pushes FaceDirection; the hook pops on the
//      first input and the START line prints the rotation mode before, right after, and at end of frame.)
//
// Copyright (C) 2026 littleRabbit6. GPL-3.0-or-later.

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <DynamicOutput/DynamicOutput.hpp>
#include <Input/KeyDef.hpp>
#include <Mod/CppUserModBase.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectArray.hpp>
#include <Unreal/UObjectGlobals.hpp>

using namespace RC;
using namespace RC::Unreal;

namespace
{
    constexpr size_t EXPECTED_SLOT = 267; // 0x858 / 8 on the 2026-09-10 build
    constexpr uint8_t FACE_DIRECTION = 2;  // ERebelRotationMode
    constexpr uint8_t PUSH_PRIORITY = 200;

    // FVector is three doubles in UE5 and goes by pointer to a caller copy; the float lands in xmm2.
    using AddMovementInputFn = void(__fastcall*)(void* self, const double* direction, float scale, bool force);

    AddMovementInputFn g_original = nullptr;
    uintptr_t** g_vtable_entry = nullptr;
    std::atomic<void*> g_player{nullptr};

    // Written by the hook and read by the engine tick. Both run on the game thread.
    struct Frame
    {
        int calls = 0;
        bool input = false;
        double accel_at_hook = 0;
        int mode_at_hook = -1;
        int mode_after_pop = -1;
        bool popped = false;
    };
    Frame g_frame;

    // Resolved on the game thread from the player's movement component; the hook reads them only for the player.
    UObject* g_cmc = nullptr;
    UFunction* g_pop = nullptr;
    int32_t g_pop_handle_at = -1;
    int32_t g_handle = -1; // the F7 test push, -1 when none

    auto read_mode(UObject* cmc) -> int
    {
        auto* mode = cmc ? cmc->GetValuePtrByPropertyNameInChain<uint8_t>(STR("CurrentRotationMode")) : nullptr;
        return mode ? *mode : -1;
    }

    auto read_len2d(UObject* object, const TCHAR* name) -> double
    {
        auto* v = object ? object->GetValuePtrByPropertyNameInChain<double>(name) : nullptr;
        return v ? std::sqrt(v[0] * v[0] + v[1] * v[1]) : -1.0;
    }

    auto pop_test_handle() -> bool
    {
        if (g_handle < 0 || !g_cmc || !g_pop || g_pop_handle_at < 0) return false;
        uint8_t params[32]{};
        std::memcpy(params + g_pop_handle_at, &g_handle, sizeof(g_handle));
        g_cmc->ProcessEvent(g_pop, params);
        g_handle = -1;
        return true;
    }

    void __fastcall add_movement_input_hook(void* self, const double* direction, float scale, bool force)
    {
        if (self == g_player.load(std::memory_order_relaxed))
        {
            ++g_frame.calls;
            const bool nonzero = direction && scale != 0.0f && (direction[0] != 0.0 || direction[1] != 0.0 || direction[2] != 0.0);
            if (nonzero && !g_frame.input)
            {
                g_frame.input = true;
                g_frame.accel_at_hook = read_len2d(g_cmc, STR("Acceleration"));
                g_frame.mode_at_hook = read_mode(g_cmc);
                if (pop_test_handle())
                {
                    g_frame.popped = true;
                    g_frame.mode_after_pop = read_mode(g_cmc);
                }
            }
        }
        g_original(self, direction, scale, force);
    }

    // The last `call qword ptr [reg+disp]` before the thunk's ret, as a vtable slot.
    auto slot_from_thunk(const uint8_t* code) -> int
    {
        int slot = -1;
        __try
        {
            for (int i = 0; i < 0x400; ++i)
            {
                if (code[i] == 0xC3 && code[i + 1] == 0xCC) break; // ret, then int3 padding
                if (code[i] != 0xFF) continue;
                const uint8_t modrm = code[i + 1];
                const bool call = ((modrm >> 3) & 7) == 2;
                const uint8_t rm = modrm & 7;
                if (!call || rm == 4 || rm == 5) continue; // SIB and rip-relative forms are not vtable calls
                if ((modrm >> 6) == 2)
                {
                    int32_t disp;
                    std::memcpy(&disp, code + i + 2, sizeof(disp));
                    if (disp > 0 && disp % 8 == 0) slot = disp / 8;
                }
                else if ((modrm >> 6) == 1)
                {
                    const int8_t disp = static_cast<int8_t>(code[i + 2]);
                    if (disp > 0 && disp % 8 == 0) slot = disp / 8;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
        return slot;
    }

    struct LiveRef
    {
        UObject* object = nullptr;
        int32_t index = -1;
        UClass* cls = nullptr;

        static auto of(UObject* live) -> LiveRef
        {
            return live ? LiveRef{live, live->GetInternalIndex(), live->GetClassPrivate()} : LiveRef{};
        }

        auto alive() const -> bool
        {
            if (!object || index < 0) return false;
            static constexpr auto DEAD = static_cast<EInternalObjectFlags>((1 << 28) | (1 << 21));
            auto* item = FUObjectArray::IndexToObject(index);
            return item && item->GetUObject() == object && !item->HasAnyFlags(DEAD) && object->GetClassPrivate() == cls;
        }
    };
} // namespace

class DWTurnInPlace : public RC::CppUserModBase
{
  public:
    DWTurnInPlace()
    {
        ModName = STR("DWTurnInPlace");
        ModVersion = STR("0.0.1-spike");
        ModDescription = STR("Turn in place, phase 0 input hook spike");
        ModAuthors = STR("littleRabbit6");
        Output::send<LogLevel::Normal>(STR("[DWTurnInPlace] v{} loaded (spike: logs only, F7 pushes FaceDirection for one pop test)\n"), ModVersion);
    }

    ~DWTurnInPlace() override
    {
        for (auto id : m_callbacks) Hook::UnregisterCallback(id);
        g_player.store(nullptr);
        if (g_vtable_entry && g_original)
        {
            DWORD prev{};
            if (VirtualProtect(g_vtable_entry, sizeof(*g_vtable_entry), PAGE_READWRITE, &prev))
            {
                if (*g_vtable_entry == reinterpret_cast<uintptr_t*>(&add_movement_input_hook)) *g_vtable_entry = reinterpret_cast<uintptr_t*>(g_original);
                VirtualProtect(g_vtable_entry, sizeof(*g_vtable_entry), prev, &prev);
            }
        }
    }

    auto on_unreal_init() -> void override
    {
        if (!install_hook()) return;
        Hook::FCallbackOptions options{false, true, STR("DWTurnInPlace"), STR("")};
        auto id = Hook::RegisterEngineTickPostCallback([this](auto&, UEngine*, float, bool) { on_engine_tick(); }, options);
        if (id != Hook::ERROR_ID) m_callbacks.push_back(id);
        // Keybinds fire off the game thread: they only raise a flag the engine tick acts on.
        register_keydown_event(Input::Key::F7, [this]() { m_push_requested.store(true); });
    }

  private:
    std::vector<Hook::GlobalCallbackId> m_callbacks;
    std::atomic<bool> m_push_requested{false};
    LiveRef m_controller;
    uint64_t m_frame = 0;
    uint64_t m_next_find = 0;
    bool m_was_input = false;
    int m_follow = 0; // frames still logged after a START
    uint64_t m_moving_without_hook = 0;
    uint64_t m_start_frame = 0;

    auto install_hook() -> bool
    {
        auto* function = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Engine.Pawn:AddMovementInput"));
        auto* pawn_cdo = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__Pawn"));
        auto* player_cdo = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Dawnwalker.Default__DawnwalkerPlayerCharacter"));
        if (!function || !pawn_cdo || !player_cdo)
        {
            Output::send<LogLevel::Error>(STR("[DWTurnInPlace] AddMovementInput, Pawn or DawnwalkerPlayerCharacter not found, inactive\n"));
            return false;
        }
        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const auto* thunk = reinterpret_cast<const uint8_t*>(function->GetFuncPtr());
        const int slot = slot_from_thunk(thunk);
        Output::send<LogLevel::Normal>(STR("[DWTurnInPlace] execAddMovementInput at +0x{:X}, vtable slot {} (expected {})\n"),
                                       reinterpret_cast<uintptr_t>(thunk) - base, slot, EXPECTED_SLOT);
        if (slot < 100 || slot > 600)
        {
            Output::send<LogLevel::Error>(STR("[DWTurnInPlace] no vtable call found in the thunk, inactive\n"));
            return false;
        }

        auto** pawn_vtable = *reinterpret_cast<uintptr_t***>(pawn_cdo);
        auto** vtable = *reinterpret_cast<uintptr_t***>(player_cdo);
        Output::send<LogLevel::Normal>(STR("[DWTurnInPlace] Pawn slot {} +0x{:X}, DawnwalkerPlayerCharacter slot {} +0x{:X} ({})\n"), slot,
                                       reinterpret_cast<uintptr_t>(pawn_vtable[slot]) - base, slot, reinterpret_cast<uintptr_t>(vtable[slot]) - base,
                                       pawn_vtable[slot] == vtable[slot] ? STR("inherited") : STR("overridden"));

        auto** entry = &vtable[slot];
        DWORD prev{};
        if (!VirtualProtect(entry, sizeof(*entry), PAGE_READWRITE, &prev))
        {
            Output::send<LogLevel::Error>(STR("[DWTurnInPlace] vtable protect failed, inactive\n"));
            return false;
        }
        g_original = reinterpret_cast<AddMovementInputFn>(*entry);
        *entry = reinterpret_cast<uintptr_t*>(&add_movement_input_hook);
        VirtualProtect(entry, sizeof(*entry), prev, &prev);
        g_vtable_entry = entry;
        Output::send<LogLevel::Normal>(STR("[DWTurnInPlace] AddMovementInput hooked (slot {})\n"), slot);
        return true;
    }

    auto resolve_player() -> void
    {
        if (!m_controller.alive())
        {
            g_player.store(nullptr);
            g_cmc = nullptr;
            if (m_frame < m_next_find) return;
            m_next_find = m_frame + 120; // FindFirstOf walks the object array: every ~2 s until found
            m_controller = LiveRef::of(UObjectGlobals::FindFirstOf(STR("BP_PlayerController_C")));
            if (!m_controller.alive()) return;
        }
        auto** pawn = m_controller.object->GetValuePtrByPropertyNameInChain<UObject*>(STR("Pawn"));
        UObject* player = pawn ? *pawn : nullptr;
        if (player != g_player.load())
        {
            g_player.store(player);
            auto** cmc = player ? player->GetValuePtrByPropertyNameInChain<UObject*>(STR("CharacterMovement")) : nullptr;
            g_cmc = cmc ? *cmc : nullptr;
            g_pop = g_cmc ? g_cmc->GetFunctionByNameInChain(STR("PopRotationMode")) : nullptr;
            auto* handle = g_pop ? g_pop->FindProperty(FName(STR("Handle"), FNAME_Find)) : nullptr;
            g_pop_handle_at = handle ? handle->GetOffset_Internal() : -1;
            Output::send<LogLevel::Normal>(STR("[DWTurnInPlace] player {} (movement {}, pop handle at {})\n"),
                                           player ? player->GetFullName() : STR("none"), g_cmc ? STR("found") : STR("missing"), g_pop_handle_at);
        }
    }

    auto push_test() -> void
    {
        if (!g_cmc || g_handle >= 0) return;
        auto* push = g_cmc->GetFunctionByNameInChain(STR("PushRotationMode"));
        auto* mode = push ? push->FindProperty(FName(STR("RotationMode"), FNAME_Find)) : nullptr;
        auto* priority = push ? push->FindProperty(FName(STR("Priority"), FNAME_Find)) : nullptr;
        auto* ret = push ? push->FindProperty(FName(STR("ReturnValue"), FNAME_Find)) : nullptr;
        if (!mode || !priority || !ret)
        {
            Output::send<LogLevel::Warning>(STR("[DWTurnInPlace] PushRotationMode or its parameters not found\n"));
            return;
        }
        uint8_t params[32]{};
        params[mode->GetOffset_Internal()] = FACE_DIRECTION;
        params[priority->GetOffset_Internal()] = PUSH_PRIORITY;
        g_cmc->ProcessEvent(push, params);
        std::memcpy(&g_handle, params + ret->GetOffset_Internal(), sizeof(g_handle));
        Output::send<LogLevel::Normal>(STR("[DWTurnInPlace] F7: FaceDirection pushed, handle {}; the first movement input pops it\n"), g_handle);
    }

    auto on_engine_tick() -> void
    {
        ++m_frame;
        resolve_player();
        if (m_push_requested.exchange(false)) push_test();

        const Frame f = g_frame;
        g_frame = Frame{};
        if (!g_cmc) return;

        const double accel = read_len2d(g_cmc, STR("Acceleration"));
        const double speed = read_len2d(g_cmc, STR("Velocity"));
        const int mode = read_mode(g_cmc);

        if (f.input && !m_was_input)
        {
            m_start_frame = m_frame;
            m_follow = 3;
            Output::send<LogLevel::Normal>(
                STR("[DWTurnInPlace] START frame {}: calls {}, accel hook {:.1f} -> end {:.1f}, speed end {:.1f}, mode hook {} -> after pop {} -> end {}{}\n"),
                m_frame, f.calls, f.accel_at_hook, accel, speed, f.mode_at_hook, f.mode_after_pop, mode, f.popped ? STR(" (popped in hook)") : STR(""));
        }
        else if (m_follow > 0)
        {
            --m_follow;
            Output::send<LogLevel::Normal>(STR("[DWTurnInPlace]   +{}: calls {}, input {}, accel {:.1f}, speed {:.1f}, mode {}\n"), m_frame - m_start_frame, f.calls,
                                           f.input, accel, speed, mode);
        }
        else if (!f.input && m_was_input)
        {
            Output::send<LogLevel::Normal>(STR("[DWTurnInPlace] STOP frame {} after {} frames of input\n"), m_frame, m_frame - m_start_frame);
        }

        if (!f.input && accel > 1.0 && ++m_moving_without_hook % 60 == 1)
        {
            Output::send<LogLevel::Warning>(STR("[DWTurnInPlace] moving without hook: {} frames with acceleration {:.1f} and no hooked input\n"),
                                            m_moving_without_hook, accel);
        }
        m_was_input = f.input;
    }
};

#define DWTURNINPLACE_API __declspec(dllexport)
extern "C"
{
    DWTURNINPLACE_API RC::CppUserModBase* start_mod()
    {
        return new DWTurnInPlace();
    }

    DWTURNINPLACE_API void uninstall_mod(RC::CppUserModBase* mod)
    {
        delete mod;
    }
}
