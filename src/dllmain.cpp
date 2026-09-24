// DWTurnInPlace: the standing character turns on the spot toward the camera with the game's own turn in place.
// While the camera sits past turn_angle from the facing and has settled, FaceDirection is pushed on the movement
// component's rotation stack and the game's anim graph plays the turn; the push is popped when the turn ends, or
// on the first movement input from inside APawn::AddMovementInput, so a walk-off never starts as a strafe.
// Design and measurements: docs/design.md.
//
// AddMovementInput is virtual: its exec thunk ends in `call qword ptr [rax+858h]` (slot 267 on the 2026-09-10
// build). The slot is read out of the thunk at load, cross-checked against 267, and hooked on
// DawnwalkerPlayerCharacter's vtable (every BP_PlayerCharacter_C shares it).
//
// Copyright (C) 2026 littleRabbit6. GPL-3.0-or-later.

#include "config.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <string>
#include <utility>
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
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectArray.hpp>
#include <Unreal/UObjectGlobals.hpp>

using namespace RC;
using namespace RC::Unreal;

namespace
{
    constexpr size_t EXPECTED_SLOT = 267; // 0x858 / 8 on the 2026-09-10 build
    constexpr uint8_t FACE_VELOCITY = 1;  // ERebelRotationMode
    constexpr uint8_t FACE_DIRECTION = 2;
    constexpr uint8_t PUSH_PRIORITY = 1; // above the base entry (0)
    constexpr uint8_t MOVE_WALKING = 1;  // EMovementMode
    constexpr size_t PARAMS_MAX = 32;    // every UFunction called here takes 16 bytes or less

    constexpr double IDLE_SPEED = 5.0;      // cm/s, 2D: a push starts only below this
    constexpr double WALK_OFF_SPEED = 50.0; // cm/s, 2D: a held push is dropped above this
    constexpr double NO_TURN_AFTER = 1.0;   // s held without the game starting a turn
    constexpr double RETRY_AFTER = 1.0;     // s after a no_turn pop before the next push
    constexpr double SAFETY_AFTER = 20.0;
    constexpr double FINISH_HOLD = 0.1; // s a turn must read as over: a turn near 180° that re-targets to the other side reads over for a tick
    constexpr double IGNORED_EVERY = 0.25; // s between IsMoveInputIgnored calls while held
    constexpr double RATE_SMOOTHING = 0.1; // s, time constant of the camera speed filter
    constexpr double HELD_LOG_EVERY = 1.0;
    constexpr uint64_t FIND_EVERY_MS = 2000; // FindFirstOf walks the whole object array
    constexpr uint64_t SETTINGS_EVERY_MS = 1000;

    // FVector is three doubles in UE5 and goes by pointer to a caller copy; the float lands in xmm2.
    using AddMovementInputFn = void(__fastcall*)(void* self, const double* direction, float scale, bool force);

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

    // The DLL is pinned (see the constructor), so a Ctrl+R restarts the mod on the same image and these keep the
    // previous instance's values: reset_state() puts the per-instance ones back. g_original and g_vtable_entry stay:
    // they describe the hook code, which is still reachable through any mod that hooked the slot after us.
    AddMovementInputFn g_original = nullptr;
    uintptr_t** g_vtable_entry = nullptr;
    std::atomic<void*> g_player{nullptr}; // null unless every offset the tick and the hook use is cached

    // Shared by the hook and the engine tick; both run on the game thread.
    LiveRef g_cmc;
    UFunction* g_pop = nullptr;
    int32_t g_pop_handle_at = -1;
    int32_t g_pop_ret_at = -1;
    int32_t g_handle = -1;       // the held FaceDirection push, -1 when none
    bool g_input = false;        // the player had movement input since the last tick
    bool g_input_popped = false; // the hook popped the held push for that input
    bool g_input_pop_ok = false;

    auto reset_state() -> void
    {
        g_player.store(nullptr);
        g_cmc = {};
        g_pop = nullptr;
        g_pop_handle_at = -1;
        g_pop_ret_at = -1;
        g_handle = -1;
        g_input = false;
        g_input_popped = false;
        g_input_pop_ok = false;
    }

    template <typename T>
    auto field(void* base, int32_t at) -> T&
    {
        return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + at);
    }

    // Returns PopRotationMode's result; the handle is forgotten either way.
    auto pop_handle() -> bool
    {
        uint8_t params[PARAMS_MAX]{};
        std::memcpy(params + g_pop_handle_at, &g_handle, sizeof(g_handle));
        g_cmc.object->ProcessEvent(g_pop, params);
        g_handle = -1;
        return g_pop_ret_at < 0 || params[g_pop_ret_at] != 0;
    }

    // Runs before the movement component's tick in the same frame, so a pop here lands before the start is built.
    void __fastcall add_movement_input_hook(void* self, const double* direction, float scale, bool force)
    {
        if (self == g_player.load(std::memory_order_relaxed) && direction && scale != 0.0f &&
            (direction[0] != 0.0 || direction[1] != 0.0 || direction[2] != 0.0))
        {
            g_input = true;
            if (g_handle >= 0 && g_cmc.alive())
            {
                g_input_pop_ok = pop_handle();
                g_input_popped = true;
            }
        }
        g_original(self, direction, scale, force);
    }

    // Keeps this DLL mapped for the life of the process. UE4SS's hot reload (Ctrl+R) destroys the mod, unloads the
    // DLL and loads it again; its callback garbage collector frees the tick callback's std::function later, on
    // its own thread, and would read a vtable out of an unmapped image. Pinned, the reload reuses this image.
    auto pin_module() -> bool
    {
        HMODULE self{};
        return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                  reinterpret_cast<LPCWSTR>(&add_movement_input_hook), &self) != 0;
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

    // -1 when missing or not `size` bytes wide: a changed type must not be read through the old layout.
    auto offset_of(UObject* object, const TCHAR* name, size_t size) -> int32_t
    {
        auto* property = object ? object->GetPropertyByNameInChain(name) : nullptr;
        return property && static_cast<size_t>(property->GetElementSize()) == size ? property->GetOffset_Internal() : -1;
    }

    auto object_at(UObject* object, const TCHAR* name) -> UObject*
    {
        const int32_t at = offset_of(object, name, sizeof(UObject*));
        return at >= 0 ? field<UObject*>(object, at) : nullptr;
    }

    auto param(UFunction* function, const TCHAR* name) -> int32_t
    {
        if (!function || function->GetParmsSize() > PARAMS_MAX) return -1;
        auto* property = function->FindProperty(FName(name, FNAME_Find));
        return property ? property->GetOffset_Internal() : -1;
    }

    // The anim instance's flags may be bitfields: read through the property's byte offset and mask, not a raw byte.
    struct BoolField
    {
        int32_t at = -1;
        uint8_t mask = 0;

        static auto of(UObject* object, const TCHAR* name) -> BoolField
        {
            auto* property = CastField<FBoolProperty>(object ? object->GetPropertyByNameInChain(name) : nullptr);
            return property ? BoolField{property->GetOffset_Internal() + property->GetByteOffset(), property->GetFieldMask()} : BoolField{};
        }

        auto ok() const -> bool
        {
            return at >= 0 && mask != 0;
        }

        auto read(UObject* object) const -> bool
        {
            return (field<uint8_t>(object, at) & mask) != 0;
        }
    };

    auto normalize(double degrees) -> double
    {
        degrees = std::fmod(degrees + 180.0, 360.0);
        if (degrees < 0.0) degrees += 360.0;
        return degrees - 180.0;
    }

    auto parse_key(const std::string& name) -> int
    {
        if (name.size() == 1 && std::isalnum(static_cast<unsigned char>(name[0])))
        {
            return std::toupper(static_cast<unsigned char>(name[0]));
        }
        if (name.size() >= 2 && (name[0] == 'F' || name[0] == 'f'))
        {
            int n = std::atoi(name.c_str() + 1);
            if (n >= 1 && n <= 12) return 0x70 + n - 1;
        }
        return -1;
    }

    auto widen(const std::string& s) -> std::wstring
    {
        return std::wstring(s.begin(), s.end());
    }

    auto last_write(const char* path) -> uint64_t
    {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExA(path, GetFileExInfoStandard, &data)) return 0;
        return (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
    }

    auto on_off(bool on) -> const TCHAR*
    {
        return on ? STR("on") : STR("off");
    }

    // One tick's reads of the player; all plain memory, no calls.
    struct Sample
    {
        double yaw = 0;    // camera
        double offset = 0; // camera minus facing, [-180, 180]
        double speed = 0;  // 2D, cm/s
        int32_t stack = 0; // rotation stack entries
        uint8_t rotation_mode = 0;
        uint8_t movement_mode = 0;
        UObject* profile = nullptr;
        float tip = 0; // TurnInPlaceAngle
        float rta = 0; // RemainingTurnAngle
        bool idle = false;
        bool crouching = false;
        bool on_ground = false;
        bool root_motion = false;
    };
} // namespace

class DWTurnInPlace : public RC::CppUserModBase
{
  public:
    DWTurnInPlace()
    {
        ModName = STR("DWTurnInPlace");
        ModVersion = STR("0.2.0");
        ModDescription = STR("Turn in place with the game's own turn animations");
        ModAuthors = STR("littleRabbit6");
        if (!pin_module())
        {
            Output::send<LogLevel::Warning>(STR("[DWTurnInPlace] could not pin the DLL (error {}): a hot reload (Ctrl+R) may crash\n"), GetLastError());
        }
        reset_state();
        m_settings_stamp = last_write(dwtip::SETTINGS_PATH);
        add_missing_keys();
        load_settings(false);
        Output::send<LogLevel::Normal>(STR("[DWTurnInPlace] v{} loaded, {}\n"), ModVersion, settings_line());
    }

    // UnregisterCallback waits for a running tick, so the tick is out before the pop and the restore.
    // The pop runs on the unloading thread: it only touches the movement component while LiveRef says it is alive.
    ~DWTurnInPlace() override
    {
        for (auto id : m_callbacks) Hook::UnregisterCallback(id);
        g_player.store(nullptr);
        if (g_handle >= 0 && g_pop && g_cmc.alive()) pop_handle();
        g_handle = -1;
        if (g_vtable_entry && g_original)
        {
            DWORD prev{};
            if (VirtualProtect(g_vtable_entry, sizeof(*g_vtable_entry), PAGE_READWRITE, &prev))
            {
                // Only this mod's own entry is put back: another mod hooked after us would otherwise be unhooked too.
                if (*g_vtable_entry == reinterpret_cast<uintptr_t*>(&add_movement_input_hook))
                {
                    *g_vtable_entry = reinterpret_cast<uintptr_t*>(g_original);
                    g_vtable_entry = nullptr;
                }
                else
                {
                    Output::send<LogLevel::Warning>(STR("[DWTurnInPlace] unload: the AddMovementInput slot no longer holds this mod's hook, left as is\n"));
                }
                VirtualProtect(g_vtable_entry, sizeof(*g_vtable_entry), prev, &prev);
            }
        }
    }

    auto on_unreal_init() -> void override
    {
        if (!install_hook()) return;
        resolve_pause();
        Hook::FCallbackOptions options{false, true, STR("DWTurnInPlace"), STR("")};
        auto id = Hook::RegisterEngineTickPostCallback([this](auto&, UEngine*, float delta, bool) { on_engine_tick(delta); }, options);
        if (id != Hook::ERROR_ID) m_callbacks.push_back(id);
        bind_toggle();
    }

  private:
    struct Held
    {
        double since = 0; // reset on a chained turn
        double pushed = 0;
        double next_log = 0;
        double next_ignored_check = 0;
        double finished_for = 0;
        UObject* profile = nullptr;
        int32_t stack_after_push = 0;
        bool saw_turn = false;
    };

    std::vector<Hook::GlobalCallbackId> m_callbacks;

    // Settings. m_enabled is also flipped by the toggle key, which runs off the game thread.
    dwtip::Settings m_settings;
    std::atomic<bool> m_enabled{true};
    std::atomic<bool> m_toggled{false};
    std::string m_bound_key;
    uint64_t m_settings_stamp = 0;
    uint64_t m_next_settings_check = 0;

    UObject* m_statics = nullptr;
    UFunction* m_is_paused = nullptr;
    int32_t m_paused_world_at = -1;
    int32_t m_paused_ret_at = -1;

    // Player, cached on each player change.
    LiveRef m_controller;
    uint64_t m_next_find = 0;
    int32_t m_pawn_at = -1;
    int32_t m_control_rotation_at = -1;
    LiveRef m_player;
    bool m_player_ok = false;
    LiveRef m_mesh;
    LiveRef m_root;
    int32_t m_relative_rotation_at = -1;
    UFunction* m_move_ignored = nullptr;
    int32_t m_move_ignored_ret = -1;
    UFunction* m_get_anim = nullptr;
    int32_t m_get_anim_ret = -1;
    int32_t m_rotation_mode_at = -1;
    int32_t m_stack_at = -1;
    int32_t m_profile_at = -1;
    int32_t m_movement_mode_at = -1;
    int32_t m_velocity_at = -1;
    UFunction* m_push = nullptr;
    int32_t m_push_mode_at = -1;
    int32_t m_push_priority_at = -1;
    int32_t m_push_ret_at = -1;

    // Anim instance, re-resolved when it dies; offsets re-cached when its class changes.
    LiveRef m_anim;
    UClass* m_anim_class = nullptr;
    bool m_anim_ok = false;
    int32_t m_tip_at = -1;
    int32_t m_rta_at = -1;
    BoolField m_idle;
    BoolField m_crouching;
    BoolField m_on_ground;
    BoolField m_root_motion;

    // State, in game seconds (the clock stops under a pause).
    double m_time = 0;
    double m_last_yaw = 0;
    bool m_have_yaw = false;
    double m_settled = 0;
    double m_retry_at = 0;
    bool m_was_input = false;
    const TCHAR* m_blocked = nullptr; // the last logged push_blocker reason
    double m_camera_rate = 0;         // degrees per second, last tick
    std::wstring m_detail;            // formatted pop detail, built only when a pop is logged
    Held m_held;

    auto settings_line() const -> std::wstring
    {
        const auto& s = m_settings;
        return std::format(STR("enabled={} turn_angle={:g} settle_speed={:g} settle_time={:g} cancel_speed={:g} chain_turns={} toggle_key={} verbose_log={}"),
                           m_enabled.load(), s.turn_angle, s.settle_speed, s.settle_time, s.cancel_speed, s.chain_turns,
                           s.toggle_key.empty() ? std::wstring(STR("none")) : widen(s.toggle_key), s.verbose ? 1 : 0);
    }

    // Startup only, before the first load: if the file exists and lacks any of the 8 keys, append each as its
    // shipped default line and refresh the stamp so the poll does not read the mod's own write as an external
    // change. A missing file is left alone: defaults apply, and load_settings logs its own warning for that.
    auto add_missing_keys() -> void
    {
        auto content = dwtip::read_file(dwtip::SETTINGS_PATH);
        if (!content) return;
        auto [updated, added] = dwtip::with_missing_keys(*content);
        if (added.empty()) return;
        if (!dwtip::write_file(dwtip::SETTINGS_PATH, updated))
        {
            Output::send<LogLevel::Warning>(STR("[DWTurnInPlace] turninplace.ini: could not add {} missing keys\n"), added.size());
            return;
        }
        m_settings_stamp = last_write(dwtip::SETTINGS_PATH);
        std::string names;
        for (auto& key : added) names += (names.empty() ? "" : ", ") + key;
        Output::send<LogLevel::Normal>(STR("[DWTurnInPlace] turninplace.ini: added {} missing keys: {}\n"), added.size(), widen(names));
    }

    auto load_settings(bool reload) -> bool
    {
        auto content = dwtip::read_file(dwtip::SETTINGS_PATH);
        if (!content)
        {
            if (reload) return false;
            Output::send<LogLevel::Warning>(STR("[DWTurnInPlace] {} not found, defaults\n"), widen(dwtip::SETTINGS_PATH));
            m_enabled.store(m_settings.enabled);
            return true;
        }
        auto parsed = dwtip::parse_settings(*content);
        if (!parsed.defaulted.empty())
        {
            Output::send<LogLevel::Warning>(STR("[DWTurnInPlace] turninplace.ini: defaults for {}\n"), widen(parsed.defaulted));
        }
        // A reload applies `enabled` only when the file's value changed: an edit to another key keeps a toggle-key state.
        const bool apply_enabled = !reload || parsed.settings.enabled != m_settings.enabled;
        m_settings = std::move(parsed.settings);
        if (apply_enabled) m_enabled.store(m_settings.enabled);
        return true;
    }

    auto poll_settings() -> void
    {
        const uint64_t now = GetTickCount64();
        if (now < m_next_settings_check) return;
        m_next_settings_check = now + SETTINGS_EVERY_MS;
        const uint64_t stamp = last_write(dwtip::SETTINGS_PATH);
        if (stamp == m_settings_stamp) return;
        m_settings_stamp = stamp;
        if (stamp == 0 || !load_settings(true))
        {
            // The Mod Menu's Apply does a temp-file-plus-rename: a poll can land in that gap and see the file
            // momentarily missing. m_settings_stamp is left at this poll's (likely 0) stamp, so the next poll,
            // once the file is back under its new write time, still differs and reloads.
            if (m_settings.verbose) Output::send<LogLevel::Warning>(STR("[DWTurnInPlace] turninplace.ini unreadable, current settings kept\n"));
            return;
        }
        Output::send<LogLevel::Normal>(STR("[DWTurnInPlace] settings reloaded: {}\n"), settings_line());
        if (m_settings.toggle_key != m_bound_key)
        {
            Output::send<LogLevel::Normal>(STR("[DWTurnInPlace] toggle_key '{}' applies at next start (UE4SS cannot unbind keys)\n"), widen(m_settings.toggle_key));
        }
    }

    auto bind_toggle() -> void
    {
        m_bound_key = m_settings.toggle_key;
        if (m_bound_key.empty()) return;
        int key = parse_key(m_bound_key);
        if (key < 0)
        {
            Output::send<LogLevel::Warning>(STR("[DWTurnInPlace] unknown toggle_key '{}', not bound\n"), widen(m_bound_key));
            return;
        }
        // Keybinds fire off the game thread: only atomics here, the engine tick logs.
        register_keydown_event(static_cast<Input::Key>(key), [this]() {
            m_enabled.store(!m_enabled.load());
            m_toggled.store(true);
        });
    }

    auto install_hook() -> bool
    {
        auto* function = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, STR("/Script/Engine.Pawn:AddMovementInput"));
        auto* player_cdo = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Dawnwalker.Default__DawnwalkerPlayerCharacter"));
        if (!function || !player_cdo)
        {
            Output::send<LogLevel::Error>(STR("[DWTurnInPlace] AddMovementInput or DawnwalkerPlayerCharacter not found, inactive\n"));
            return false;
        }
        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const auto* thunk = reinterpret_cast<const uint8_t*>(function->GetFuncPtr());
        const int slot = slot_from_thunk(thunk);
        if (slot < 100 || slot > 600)
        {
            Output::send<LogLevel::Error>(STR("[DWTurnInPlace] no vtable call found in execAddMovementInput (+0x{:X}), inactive\n"),
                                          reinterpret_cast<uintptr_t>(thunk) - base);
            return false;
        }

        auto** vtable = *reinterpret_cast<uintptr_t***>(player_cdo);
        auto** entry = &vtable[slot];
        // On a reused image the slot can still hold this hook (the last unload left it in place): the original
        // is the one already known, never the hook itself. A foreign hook in a slot we once hooked may chain to
        // this hook, so taking it as the original would call in a loop; the mod stays inactive instead.
        if (*entry == reinterpret_cast<uintptr_t*>(&add_movement_input_hook))
        {
            if (!g_original)
            {
                Output::send<LogLevel::Error>(STR("[DWTurnInPlace] AddMovementInput slot already holds this hook and the original is unknown, inactive\n"));
                return false;
            }
            g_vtable_entry = entry;
            Output::send<LogLevel::Normal>(STR("[DWTurnInPlace] AddMovementInput hooked, vtable slot {} (expected {}), hook still in place from the last load\n"),
                                           slot, EXPECTED_SLOT);
            return true;
        }
        if (g_original && *entry != reinterpret_cast<uintptr_t*>(g_original))
        {
            Output::send<LogLevel::Error>(STR("[DWTurnInPlace] AddMovementInput slot holds another hook installed over this mod's, inactive\n"));
            return false;
        }
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
        Output::send<LogLevel::Normal>(STR("[DWTurnInPlace] AddMovementInput hooked, vtable slot {} (expected {}), execAddMovementInput at +0x{:X}\n"), slot,
                                       EXPECTED_SLOT, reinterpret_cast<uintptr_t>(thunk) - base);
        return true;
    }

    auto resolve_pause() -> void
    {
        m_statics = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, STR("/Script/Engine.Default__GameplayStatics"));
        m_is_paused = m_statics ? m_statics->GetFunctionByNameInChain(STR("IsGamePaused")) : nullptr;
        m_paused_world_at = param(m_is_paused, STR("WorldContextObject"));
        m_paused_ret_at = param(m_is_paused, STR("ReturnValue"));
        if (m_paused_world_at < 0 || m_paused_ret_at < 0)
        {
            m_is_paused = nullptr;
            Output::send<LogLevel::Warning>(STR("[DWTurnInPlace] GameplayStatics.IsGamePaused not found, pause not detected\n"));
        }
    }

    auto paused() -> bool
    {
        if (!m_is_paused) return false;
        uint8_t params[PARAMS_MAX]{};
        field<UObject*>(params, m_paused_world_at) = m_player.object;
        m_statics->ProcessEvent(m_is_paused, params);
        return params[m_paused_ret_at] != 0;
    }

    auto move_input_ignored() -> bool
    {
        uint8_t params[PARAMS_MAX]{};
        m_player.object->ProcessEvent(m_move_ignored, params);
        return params[m_move_ignored_ret] != 0;
    }

    // Pops the held handle if the movement component is alive; a dead one is forgotten, never called.
    auto release(const TCHAR* reason, const TCHAR* detail, const Sample* s) -> void
    {
        if (g_handle < 0) return;
        if (!g_cmc.alive())
        {
            g_handle = -1;
            if (m_settings.verbose) Output::send<LogLevel::Normal>(STR("[DWTurnInPlace] handle forgotten ({}): the movement component is gone\n"), reason);
            return;
        }
        log_pop(reason, detail, s, pop_handle());
    }

    auto log_pop(const TCHAR* reason, const TCHAR* detail, const Sample* s, bool ok) -> void
    {
        if (!m_settings.verbose) return;
        const TCHAR* refused = ok ? STR("") : STR(" (PopRotationMode returned false)");
        if (s)
        {
            Output::send<LogLevel::Normal>(STR("[DWTurnInPlace] pop {}{} held={:.2f}s saw_turn={} offset={:.0f} speed={:.0f} idle={}{}\n"), reason, detail,
                                           m_time - m_held.pushed, m_held.saw_turn, s->offset, s->speed, s->idle, refused);
        }
        else
        {
            Output::send<LogLevel::Normal>(STR("[DWTurnInPlace] pop {}{} held={:.2f}s saw_turn={}{}\n"), reason, detail, m_time - m_held.pushed, m_held.saw_turn,
                                           refused);
        }
    }

    auto set_player(UObject* pawn) -> void
    {
        release(STR("lost"), STR(""), nullptr);
        g_player.store(nullptr);
        g_cmc = {};
        g_pop = nullptr;
        m_player = LiveRef::of(pawn);
        m_player_ok = false;
        m_mesh = m_root = m_anim = {};
        m_anim_class = nullptr;
        m_anim_ok = false;
        m_have_yaw = false;
        m_settled = 0;
        if (!pawn)
        {
            Output::send<LogLevel::Normal>(STR("[DWTurnInPlace] player lost\n"));
            return;
        }

        auto* cmc = object_at(pawn, STR("CharacterMovement"));
        auto* mesh = object_at(pawn, STR("Mesh"));
        auto* root = object_at(pawn, STR("RootComponent"));
        m_move_ignored = pawn->GetFunctionByNameInChain(STR("IsMoveInputIgnored"));
        m_move_ignored_ret = param(m_move_ignored, STR("ReturnValue"));
        m_get_anim = mesh ? mesh->GetFunctionByNameInChain(STR("GetAnimInstance")) : nullptr;
        m_get_anim_ret = param(m_get_anim, STR("ReturnValue"));
        m_relative_rotation_at = offset_of(root, STR("RelativeRotation"), 3 * sizeof(double));
        m_rotation_mode_at = offset_of(cmc, STR("CurrentRotationMode"), sizeof(uint8_t));
        m_stack_at = offset_of(cmc, STR("RotationModeStack"), 16); // FScriptArray {void* data; int32 Num; int32 Max}
        m_profile_at = offset_of(cmc, STR("CurrentMovementProfile"), sizeof(UObject*));
        m_movement_mode_at = offset_of(cmc, STR("MovementMode"), sizeof(uint8_t));
        m_velocity_at = offset_of(cmc, STR("Velocity"), 3 * sizeof(double));
        m_push = cmc ? cmc->GetFunctionByNameInChain(STR("PushRotationMode")) : nullptr;
        m_push_mode_at = param(m_push, STR("RotationMode"));
        m_push_priority_at = param(m_push, STR("Priority"));
        m_push_ret_at = param(m_push, STR("ReturnValue"));
        auto* pop = cmc ? cmc->GetFunctionByNameInChain(STR("PopRotationMode")) : nullptr;
        const int32_t pop_handle_at = param(pop, STR("Handle"));
        const int32_t pop_ret_at = param(pop, STR("ReturnValue"));

        std::wstring missing;
        auto need = [&](bool ok, const TCHAR* what) {
            if (ok) return;
            if (!missing.empty()) missing += STR(", ");
            missing += what;
        };
        need(cmc, STR("CharacterMovement"));
        need(mesh, STR("Mesh"));
        need(root, STR("RootComponent"));
        need(m_move_ignored_ret >= 0, STR("IsMoveInputIgnored"));
        need(m_get_anim_ret >= 0, STR("GetAnimInstance"));
        need(m_relative_rotation_at >= 0, STR("RelativeRotation"));
        need(m_rotation_mode_at >= 0, STR("CurrentRotationMode"));
        need(m_stack_at >= 0, STR("RotationModeStack"));
        need(m_profile_at >= 0, STR("CurrentMovementProfile"));
        need(m_movement_mode_at >= 0, STR("MovementMode"));
        need(m_velocity_at >= 0, STR("Velocity"));
        need(m_push_mode_at >= 0 && m_push_priority_at >= 0 && m_push_ret_at >= 0, STR("PushRotationMode"));
        need(pop_handle_at >= 0, STR("PopRotationMode"));
        if (!missing.empty())
        {
            Output::send<LogLevel::Error>(STR("[DWTurnInPlace] player {}: {} not found, inactive for this player\n"), pawn->GetFullName(), missing);
            return;
        }

        m_mesh = LiveRef::of(mesh);
        m_root = LiveRef::of(root);
        g_cmc = LiveRef::of(cmc);
        g_pop = pop;
        g_pop_handle_at = pop_handle_at;
        g_pop_ret_at = pop_ret_at;
        m_player_ok = true;
        g_player.store(pawn);
        Output::send<LogLevel::Normal>(STR("[DWTurnInPlace] player {}\n"), pawn->GetFullName());
    }

    auto resolve_anim() -> bool
    {
        if (m_anim.alive()) return m_anim_ok;
        uint8_t params[PARAMS_MAX]{};
        m_mesh.object->ProcessEvent(m_get_anim, params);
        auto* anim = field<UObject*>(params, m_get_anim_ret);
        m_anim = LiveRef::of(anim);
        if (!anim) return false;
        if (m_anim.cls == m_anim_class) return m_anim_ok;

        m_anim_class = m_anim.cls;
        m_tip_at = offset_of(anim, STR("TurnInPlaceAngle"), sizeof(float));
        m_rta_at = offset_of(anim, STR("RemainingTurnAngle"), sizeof(float));
        m_idle = BoolField::of(anim, STR("bIsIdle"));
        m_crouching = BoolField::of(anim, STR("bIsCrouching"));
        m_on_ground = BoolField::of(anim, STR("bIsOnGround"));
        m_root_motion = BoolField::of(anim, STR("bIsPlayingRootMotion"));
        m_anim_ok = m_tip_at >= 0 && m_rta_at >= 0 && m_idle.ok() && m_crouching.ok() && m_on_ground.ok() && m_root_motion.ok();
        if (!m_anim_ok)
        {
            Output::send<LogLevel::Error>(STR("[DWTurnInPlace] anim instance {} lacks the turn in place properties, inactive while it is up\n"),
                                          anim->GetFullName());
        }
        return m_anim_ok;
    }

    // Controller from FindFirstOf only while it is dead, at most every 2 s; the pawn through its Pawn property.
    auto resolve() -> bool
    {
        if (!m_controller.alive())
        {
            if (m_player.object) set_player(nullptr);
            m_controller = {};
            const uint64_t now = GetTickCount64();
            if (now < m_next_find) return false;
            m_next_find = now + FIND_EVERY_MS;
            auto* controller = UObjectGlobals::FindFirstOf(STR("BP_PlayerController_C"));
            if (!controller) return false;
            m_pawn_at = offset_of(controller, STR("Pawn"), sizeof(UObject*));
            m_control_rotation_at = offset_of(controller, STR("ControlRotation"), 3 * sizeof(double));
            if (m_pawn_at < 0 || m_control_rotation_at < 0)
            {
                Output::send<LogLevel::Error>(STR("[DWTurnInPlace] player controller has no Pawn or ControlRotation\n"));
                return false;
            }
            m_controller = LiveRef::of(controller);
        }
        auto* pawn = field<UObject*>(m_controller.object, m_pawn_at);
        if (pawn != m_player.object || (pawn && !m_player.alive())) set_player(pawn);
        if (!m_player_ok) return false;
        if (!g_cmc.alive() || !m_mesh.alive() || !m_root.alive())
        {
            set_player(nullptr); // re-cached from the controller next tick
            return false;
        }
        return resolve_anim();
    }

    auto sample() -> Sample
    {
        auto* cmc = g_cmc.object;
        auto* anim = m_anim.object;
        const auto* control = &field<double>(m_controller.object, m_control_rotation_at);
        const auto* actor = &field<double>(m_root.object, m_relative_rotation_at);
        const auto* velocity = &field<double>(cmc, m_velocity_at);
        Sample s;
        s.yaw = control[1];
        s.offset = normalize(control[1] - actor[1]);
        s.speed = std::sqrt(velocity[0] * velocity[0] + velocity[1] * velocity[1]);
        s.stack = field<int32_t>(cmc, m_stack_at + 8);
        s.rotation_mode = field<uint8_t>(cmc, m_rotation_mode_at);
        s.movement_mode = field<uint8_t>(cmc, m_movement_mode_at);
        s.profile = field<UObject*>(cmc, m_profile_at);
        s.tip = field<float>(anim, m_tip_at);
        s.rta = field<float>(anim, m_rta_at);
        s.idle = m_idle.read(anim);
        s.crouching = m_crouching.read(anim);
        s.on_ground = m_on_ground.read(anim);
        s.root_motion = m_root_motion.read(anim);
        return s;
    }

    // Returns true while the camera turns fast enough to cancel a held turn: cancel_speed, never below settle_speed.
    // The rate is smoothed: per-frame mouse deltas spike well above a slow pan's average, and one threshold for
    // both start and cancel made a pan near it start and cancel a turn every few frames (measured at 60 deg/s).
    auto track_settle(double yaw, double dt) -> bool
    {
        if (!m_have_yaw)
        {
            m_last_yaw = yaw;
            m_have_yaw = true;
            m_settled = 0;
            m_camera_rate = 0;
            return false;
        }
        if (dt <= 0.0) return false;
        const double rate = std::abs(normalize(yaw - m_last_yaw)) / dt;
        m_last_yaw = yaw;
        m_camera_rate += (rate - m_camera_rate) * (1.0 - std::exp(-dt / RATE_SMOOTHING));
        m_settled = m_camera_rate >= m_settings.settle_speed ? 0.0 : m_settled + dt;
        return m_camera_rate >= std::max(m_settings.cancel_speed, m_settings.settle_speed);
    }

    // The first guard that stops a push, or nullptr. Cheapest first; IsMoveInputIgnored is a UFunction call, so last.
    auto push_blocker(const Sample& s) -> const TCHAR*
    {
        if (!m_enabled.load()) return STR("disabled");
        // Combat pushes DA_Combat_MovementProfile and its own FaceDirection entry at priority 1: its turns are the game's.
        if (s.stack != 1 || s.rotation_mode != FACE_VELOCITY) return STR("rotation mode taken");
        if (s.movement_mode != MOVE_WALKING || !s.on_ground) return STR("not walking");
        // The game has no crouched FaceDirection turn: it plays the standing one from the crouch pose.
        if (s.crouching) return STR("crouched");
        if (s.speed >= IDLE_SPEED || !s.idle) return STR("not idle");
        if (s.root_motion) return STR("root motion");
        if (m_time < m_retry_at) return STR("retry cooldown");
        if (move_input_ignored()) return STR("input ignored");
        return nullptr;
    }

    auto try_push(const Sample& s) -> void
    {
        // Only a settled camera past turn_angle asks for a turn.
        if (std::abs(s.offset) <= m_settings.turn_angle || m_settled < m_settings.settle_time)
        {
            m_blocked = nullptr;
            return;
        }
        if (const TCHAR* why = push_blocker(s))
        {
            // Once per reason while the camera stays past the angle, not every tick.
            if (m_settings.verbose && why != m_blocked)
            {
                Output::send<LogLevel::Normal>(STR("[DWTurnInPlace] blocked ({}) offset={:.0f} profile={}\n"), why, s.offset,
                                               s.profile ? s.profile->GetName() : std::wstring(STR("none")));
            }
            m_blocked = why;
            return;
        }
        m_blocked = nullptr;

        uint8_t params[PARAMS_MAX]{};
        params[m_push_mode_at] = FACE_DIRECTION;
        params[m_push_priority_at] = PUSH_PRIORITY;
        g_cmc.object->ProcessEvent(m_push, params);
        const int32_t handle = field<int32_t>(params, m_push_ret_at);
        if (handle < 0)
        {
            Output::send<LogLevel::Warning>(STR("[DWTurnInPlace] PushRotationMode returned {}, retrying in {:g}s\n"), handle, RETRY_AFTER);
            m_retry_at = m_time + RETRY_AFTER;
            return;
        }
        g_handle = handle;
        m_held = Held{m_time, m_time, m_time + HELD_LOG_EVERY, m_time + IGNORED_EVERY, 0.0, s.profile, field<int32_t>(g_cmc.object, m_stack_at + 8), false};
        if (m_settings.verbose)
        {
            Output::send<LogLevel::Normal>(STR("[DWTurnInPlace] push offset={:.0f} settled={:.2f}s handle={} stack={} profile={}\n"), s.offset, m_settled,
                                           handle, m_held.stack_after_push, s.profile ? s.profile->GetName() : std::wstring(STR("none")));
        }
    }

    auto update_held(const Sample& s, double dt, bool camera_moving) -> void
    {
        auto& h = m_held;
        if (s.tip != 0.0f || s.rta != 0.0f) h.saw_turn = true;
        if (m_settings.verbose && m_time >= h.next_log)
        {
            h.next_log = m_time + HELD_LOG_EVERY;
            Output::send<LogLevel::Normal>(STR("[DWTurnInPlace] held tip={:.1f} rta={:.1f} idle={} rootMotion={} onGround={}\n"), s.tip, s.rta, s.idle,
                                           s.root_motion, s.on_ground);
        }

        // Another system pushed on top, or the context changed under the push.
        if (s.stack > 2) return release(STR("override"), STR(" (stack)"), &s);
        if (s.profile != h.profile) return release(STR("override"), STR(" (profile)"), &s);

        // bIsIdle and bIsPlayingRootMotion are not reasons here: both change during the turn itself.
        const TCHAR* left = nullptr;
        if (!m_enabled.load()) left = STR(" (disabled)");
        else if (s.movement_mode != MOVE_WALKING) left = STR(" (movement mode)");
        else if (s.speed > WALK_OFF_SPEED) left = STR(" (speed)");
        else if (s.crouching) left = STR(" (crouch)");
        else if (m_time >= h.next_ignored_check)
        {
            h.next_ignored_check = m_time + IGNORED_EVERY;
            if (move_input_ignored()) left = STR(" (input ignored)");
        }
        if (left) return release(STR("left_idle"), left, &s);

        // While FaceDirection is pushed the game re-targets the turn to the camera at once, with no wait. A fast swing
        // (above cancel_speed, smoothed) drops the push instead: the turn winds down (about 0.24 s, no snap)
        // and the next one waits the full settle_time. A slower pan keeps the turn following the camera.
        if (camera_moving)
        {
            if (m_settings.verbose) m_detail = std::format(STR(" (camera {:.0f} deg/s)"), m_camera_rate);
            return release(STR("camera"), m_settings.verbose ? m_detail.c_str() : STR(""), &s);
        }

        const double held = m_time - h.since;
        const bool finished = h.saw_turn && s.tip == 0.0f && s.rta == 0.0f && s.idle;
        h.finished_for = finished ? h.finished_for + dt : 0.0;
        if (h.finished_for >= FINISH_HOLD)
        {
            if (m_settings.chain_turns && std::abs(s.offset) > m_settings.turn_angle)
            {
                // The mode stays pushed, so the game starts the next turn itself.
                h.since = m_time;
                h.saw_turn = false;
                h.finished_for = 0.0;
                if (m_settings.verbose) Output::send<LogLevel::Normal>(STR("[DWTurnInPlace] chain offset={:.0f}\n"), s.offset);
                return;
            }
            return release(STR("done"), STR(""), &s);
        }
        if (!h.saw_turn && held >= NO_TURN_AFTER)
        {
            m_retry_at = m_time + RETRY_AFTER;
            return release(STR("no_turn"), STR(""), &s);
        }
        if (held >= SAFETY_AFTER) release(STR("safety"), STR(""), &s);
    }

    auto on_engine_tick(float delta) -> void
    {
        poll_settings();
        if (m_toggled.exchange(false)) Output::send<LogLevel::Normal>(STR("[DWTurnInPlace] toggle: {}\n"), on_off(m_enabled.load()));

        const bool input = std::exchange(g_input, false);
        const bool input_popped = std::exchange(g_input_popped, false);
        if (!resolve())
        {
            release(STR("lost"), STR(""), nullptr);
            return;
        }
        if (paused()) return; // the rotation stack does not resolve under a pause, and the clocks stop

        const double dt = delta > 0.0f ? static_cast<double>(delta) : 0.0;
        m_time += dt;
        const Sample s = sample();
        const bool camera_moving = track_settle(s.yaw, dt);
        const double settled_before = m_settled;
        if (input) m_settled = 0.0; // no push until the camera settles again after the player stops
        if (input_popped) log_pop(STR("input"), STR(""), &s, g_input_pop_ok);
        // A walk-off that began with the camera past turn_angle and nothing held: the swing-and-go case the settle skips.
        if (input && !m_was_input && !input_popped && m_settings.verbose && std::abs(s.offset) > m_settings.turn_angle)
        {
            Output::send<LogLevel::Normal>(STR("[DWTurnInPlace] skip offset={:.0f} settled={:.2f}s (moved before a turn)\n"), s.offset, settled_before);
        }
        m_was_input = input;

        if (g_handle >= 0) update_held(s, dt, camera_moving);
        else if (!input) try_push(s);
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
