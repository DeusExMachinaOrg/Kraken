#define LOGGER "testharness"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "ext/logger.hpp"
#include "routines.hpp"
#include "config.hpp"

#include "hta/CStr.hpp"
#include "hta/CVector.hpp"
#include "hta/Quaternion.hpp"
#include "hta/ai/Vehicle.hpp"
#include "hta/ai/Wheel.hpp"
#include "hta/ai/DynamicScene.hpp"
#include "hta/ai/CServer.hpp"
#include "hta/ai/ObjContainer.hpp"

#include "fix/testharness.hpp"
#include "fix/joltshadow.hpp"

namespace kraken::fix::testharness {
    namespace fs = std::filesystem;

    // Autonomous scripted-input + telemetry harness, driven entirely by files under
    // ./data/kraken_testharness/ so an external tool (no human, no keyboard/gamepad)
    // can script a vehicle, capture its world trajectory, and compare/tune against it.
    //
    // Protocol:
    //   scenario.csv     - written by the external tool before a run. Optional first
    //                      data line "spawn,x,y,z,qx,qy,qz,qw", then rows
    //                      "t,throttle,steer,brake,handbrake" sorted by t.
    //   trigger.txt      - single line token. Changing it starts a new scenario.
    //   output_<tok>.csv - telemetry written once per tick while the scenario runs.
    //   output_<tok>.done- written when the scenario finishes ("ok" / "no_vehicle").

    struct Sample {
        float t        = 0.0f;
        float throttle = 0.0f;
        float steer    = 0.0f;
        float brake    = 0.0f;
        bool  handbrake = false;
    };

    struct State {
        fs::path           baseDir;
        std::string        lastTrigger;
        std::string        token;
        std::vector<Sample> samples;
        hta::CVector        spawnPos;
        hta::Quaternion     spawnRot;
        bool                hasSpawn = false;
        bool                running  = false;
        float               clock    = 0.0f;
        std::ofstream       telemetry;
        bool                tornWheel = false; // one-shot latch for testharness_tear_wheel_at_t
    };

    static State g_state;

    // Debug-only (docs §22.4/§22.6): when [testharness] ram_test=1, StartScenario dynamically
    // computes a spawn point for the PLAYER right behind another live vehicle - found once,
    // read-only, and never otherwise touched - so a plain scenario.csv drive rams the player
    // straight into it. This reproduces an ODE-driven vehicle colliding with the Jolt-disabled
    // player body deterministically, instead of waiting for real AI/combat to do it. Only the
    // player is ever teleported, via the exact same spawn mechanism the normal (non-ram_test)
    // path already uses - an earlier version of this tried teleporting the OTHER vehicle
    // instead, which meant hand-repositioning its separate chassis+wheel ODE bodies (Hinge2-
    // jointed) and fighting whatever game-side logic keeps live AI vehicles where they're
    // supposed to be; that fought back with its own large corrective jumps every time. Letting
    // the target vehicle stay completely untouched sidesteps all of that.
    struct RamTestState {
        hta::ai::Vehicle* target = nullptr; // logged only, never dereferenced after StartScenario
    };
    static RamTestState g_ramTest;

    static std::vector<std::string> Split(const std::string& line) {
        std::vector<std::string> tokens;
        std::stringstream ss(line);
        std::string token;
        while (std::getline(ss, token, ',')) {
            tokens.push_back(token);
        }
        return tokens;
    }

    static bool LoadScenario(const fs::path& path, State& state) {
        std::ifstream in(path);
        if (!in.is_open()) {
            LOG_ERROR("Cannot open scenario file");
            return false;
        }

        state.samples.clear();
        state.hasSpawn = false;

        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;

            std::vector<std::string> tokens = Split(line);
            if (tokens.empty()) continue;

            if (tokens[0] == "spawn" && tokens.size() >= 8) {
                state.spawnPos = hta::CVector(std::stof(tokens[1]), std::stof(tokens[2]), std::stof(tokens[3]));
                state.spawnRot = hta::Quaternion(std::stof(tokens[4]), std::stof(tokens[5]), std::stof(tokens[6]), std::stof(tokens[7]));
                state.hasSpawn = true;
                continue;
            }

            if (tokens[0] == "t") continue; // header line
            if (tokens.size() < 5) continue;

            Sample sample;
            sample.t         = std::stof(tokens[0]);
            sample.throttle  = std::stof(tokens[1]);
            sample.steer     = std::stof(tokens[2]);
            sample.brake     = std::stof(tokens[3]);
            sample.handbrake = std::stoi(tokens[4]) != 0;
            state.samples.push_back(sample);
        }

        std::sort(state.samples.begin(), state.samples.end(), [](const Sample& a, const Sample& b) {
            return a.t < b.t;
        });

        return !state.samples.empty();
    }

    static const Sample* SampleAt(const State& state, float t) {
        const Sample* result = nullptr;
        for (const Sample& sample : state.samples) {
            if (sample.t <= t) result = &sample;
            else break;
        }
        if (!result && !state.samples.empty()) result = &state.samples.front();
        return result;
    }

    // Same enumeration idiom as fix::joltshadow's MirrorOtherVehicles (CServer::m_pObjects,
    // updatingBegin/End, Obj::cast<Vehicle>) - reused here rather than re-derived.
    static hta::ai::Vehicle* FindOtherLiveVehicle(hta::ai::Vehicle* exclude) {
        hta::ai::CServer* server = hta::ai::CServer::Instance();
        if (server == nullptr || server->m_pObjects == nullptr) return nullptr;

        hta::ai::ObjContainer* objects = server->m_pObjects;
        for (hta::ai::ObjContainer::iterator it = objects->updatingBegin(); it != objects->updatingEnd(); ++it) {
            hta::ai::Vehicle* vehicle = (*it)->cast<hta::ai::Vehicle>();
            if (vehicle != nullptr && vehicle != exclude)
                return vehicle;
        }
        return nullptr;
    }

    static hta::ai::Vehicle* GetTargetVehicle() {
        hta::ai::DynamicScene* scene = hta::ai::DynamicScene::Instance();
        if (!scene) return nullptr;
        return scene->GetVehicleControlledByPlayer();
    }

    static void FinishScenario(const char* reason) {
        State& state = g_state;

        hta::ai::Vehicle* vehicle = GetTargetVehicle();
        if (vehicle) {
            vehicle->m_throttle     = 0.0f;
            vehicle->m_steerRadians = 0.0f;
            vehicle->m_brake        = 1.0f;
            vehicle->m_bHandBrake   = true;
        }

        if (state.telemetry.is_open()) state.telemetry.close();

        std::ofstream done(state.baseDir / ("output_" + state.token + ".done"), std::ios::trunc);
        done << reason << "\n";

        state.running = false;
        LOG_INFO("Scenario finished");
    }

    static void StartScenario(const std::string& token) {
        State& state = g_state;

        if (!LoadScenario(state.baseDir / "scenario.csv", state)) {
            LOG_ERROR("Failed to load scenario.csv, ignoring trigger");
            return;
        }

        state.token   = token;
        state.clock   = 0.0f;
        state.running = true;
        state.tornWheel = false;

        g_ramTest.target = nullptr;

        if (kraken::Config::Instance().testharness_ram_test.value != 0) {
            hta::ai::Vehicle* player = GetTargetVehicle();
            hta::ai::Vehicle* target = player ? FindOtherLiveVehicle(player) : nullptr;

            if (!player || !target) {
                LOG_ERROR("ram_test: %s, ignoring trigger",
                    !player ? "no player vehicle" : "no other live vehicle to ram into");
                state.running = false;
                return;
            }

            g_ramTest.target = target;

            const float            offset    = kraken::Config::Instance().testharness_ram_test_offset.value;
            const hta::CVector     targetPos = target->GetPosition();
            const hta::Quaternion  targetRot = target->GetRotation();
            // Chassis-local Z-forward convention (assumed elsewhere in this codebase too, see
            // joltshadow.cpp's suspension-axis comments) - spawning the player behind the
            // target facing the same way means a plain throttle=1/steer=0 scenario.csv drives
            // it straight into the target, no pursuit steering needed. The target itself is
            // never touched here - it stays a completely normal, live vehicle throughout,
            // same as it would be if the player just happened to drive into it during real
            // play - only the player's spawn point is computed dynamically instead of coming
            // from a fixed scenario.csv "spawn" line.
            const hta::CVector forward = targetRot * hta::CVector(0.0f, 0.0f, 1.0f);

            state.spawnPos = targetPos - forward * offset;
            state.spawnRot = targetRot;
            state.hasSpawn = true;

            LOG_INFO("ram_test: will spawn player %.1fm behind other vehicle %p and drive into it per scenario.csv",
                (double) offset, (void*) target);
        }

        hta::ai::Vehicle* vehicle = GetTargetVehicle();
        if (vehicle) {
            if (state.hasSpawn) {
                vehicle->SetPositionSelf(state.spawnPos);
                vehicle->SetRotationSelf(state.spawnRot);
                // Under [jolt_harness] apply=1, ApplyJoltToVehicle overwrites this ODE-side
                // teleport again next frame unless Jolt's own shadow body is moved too - see
                // joltshadow::TeleportPlayerShadow's comment. Harmless no-op otherwise (Jolt
                // off, no shadow built yet, or this isn't the player).
                kraken::fix::joltshadow::TeleportPlayerShadow(state.spawnPos, state.spawnRot);
            }
            vehicle->SetLinearVelocity(hta::CVector(0.0f, 0.0f, 0.0f));
            vehicle->SetAngularVelocity(hta::CVector(0.0f, 0.0f, 0.0f));
            vehicle->SetThrottle(0.0f, false);
            vehicle->m_steerRadians = 0.0f;
            vehicle->SetBrake(0.0f);
            vehicle->m_bHandBrake   = false;
        }

        state.telemetry.open(state.baseDir / ("output_" + token + ".csv"), std::ios::trunc);
        state.telemetry << "t,px,py,pz,qx,qy,qz,qw,comx,comy,comz,vx,vy,vz,avx,avy,avz,throttle,steer,brake,handbrake,"
                            "gear,engineRpm,realThrottle,wheelsTouchingGround,numWheels,drivenWheels,drivenWheelsJointed\n";

        LOG_INFO("Scenario started");
    }

    // Direct C++ call to Vehicle::setGodMode/setImmortalMode (VA 0x5CCCD0/0x5CCCF0, already
    // exposed via the vendored Vehicle.hpp header) - bypasses data\scripts\cheats.lua's own
    // god() wrapper entirely, which gates on testcheat() checking GetComputerName() against a
    // hardcoded whitelist of developer machine names and would silently no-op on any other
    // machine. Self-healing every frame (checked via getGodMode(), not a one-shot flag) so it
    // survives a vehicle swap (autoload swapping in the real save, manual vehicle switch) the
    // same way other per-frame safety rails in this codebase do (e.g. wheelmodel's own gates).
    struct GodModeState {
        bool               enabled     = false;
        hta::ai::Vehicle*  lastVehicle = nullptr;
    };
    static GodModeState g_godMode;

    static void GodModeTick() {
        if (!g_godMode.enabled) return;

        hta::ai::Vehicle* vehicle = GetTargetVehicle();
        if (vehicle == nullptr) return;

        if (vehicle != g_godMode.lastVehicle) {
            LOG_INFO("God mode: applying to player vehicle %p", (void*) vehicle);
            g_godMode.lastVehicle = vehicle;
        }
        if (!vehicle->getGodMode()) {
            vehicle->setGodMode(true);
            vehicle->setImmortalMode(true);
        }
    }

    // Wall-clock (not simulated-time) frame timing, logged as a rolling-window summary every
    // perfmon_interval seconds - independent of the scripted-scenario trigger.txt state (runs
    // whenever CollideSceneHook fires, i.e. every frame regardless of whether a scenario is
    // "running"). Used for A/B comparing overall game performance with vs. without Jolt enabled
    // ([jolt] enabled=0/1) on the same save - see docs/jolt-integration-techanalysis.md §17.
    struct PerfState {
        bool                                  enabled  = false;
        double                                intervalSec = 5.0;
        bool                                  hasLast  = false;
        std::chrono::steady_clock::time_point lastFrame;
        uint64_t                              windowFrames = 0;
        double                                windowSeconds = 0.0;
        double                                windowMinMs = 0.0;
        double                                windowMaxMs = 0.0;
    };
    static PerfState g_perf;

    static void PerfTick() {
        if (!g_perf.enabled) return;

        auto now = std::chrono::steady_clock::now();
        if (!g_perf.hasLast) {
            g_perf.lastFrame = now;
            g_perf.hasLast   = true;
            return;
        }

        const double frameMs = std::chrono::duration<double, std::milli>(now - g_perf.lastFrame).count();
        g_perf.lastFrame = now;

        // Plain comparisons, not std::min/max: windows.h's min/max macros (pulled in
        // transitively via config.hpp/routines.hpp, no NOMINMAX in this translation unit)
        // would otherwise mangle those calls into a syntax error - same gotcha documented in
        // fix::jolt/fix::joltshadow, sidestepped here rather than adding NOMINMAX file-wide.
        if (g_perf.windowFrames == 0) {
            g_perf.windowMinMs = frameMs;
            g_perf.windowMaxMs = frameMs;
        } else {
            if (frameMs < g_perf.windowMinMs) g_perf.windowMinMs = frameMs;
            if (frameMs > g_perf.windowMaxMs) g_perf.windowMaxMs = frameMs;
        }
        g_perf.windowSeconds += frameMs / 1000.0;
        ++g_perf.windowFrames;

        if (g_perf.windowSeconds >= g_perf.intervalSec) {
            const double avgMs = g_perf.windowSeconds * 1000.0 / (double) g_perf.windowFrames;
            const double fps   = avgMs > 0.0 ? 1000.0 / avgMs : 0.0;
            LOG_INFO("perf: fps=%.1f avg_frame_ms=%.3f min_ms=%.3f max_ms=%.3f frames=%llu window=%.1fs",
                fps, avgMs, g_perf.windowMinMs, g_perf.windowMaxMs,
                (unsigned long long) g_perf.windowFrames, g_perf.windowSeconds);
            g_perf.windowFrames  = 0;
            g_perf.windowSeconds = 0.0;
        }
    }

    static void Tick(float dt) {
        State& state = g_state;

        std::ifstream trigger(state.baseDir / "trigger.txt");
        if (trigger.is_open()) {
            std::string token;
            std::getline(trigger, token);
            if (!token.empty() && !state.running && token != state.lastTrigger) {
                state.lastTrigger = token;
                StartScenario(token);
            }
        }

        if (!state.running) return;

        hta::ai::Vehicle* vehicle = GetTargetVehicle();
        if (!vehicle) {
            FinishScenario("no_vehicle");
            return;
        }

        // Debug-only one-shot wheel tear (docs §22.2/§22.14) - lets the UAF fix
        // (ShadowState::wheelOrder revalidation, joltshadow.cpp's ShadowWheelsStillPresent) be
        // exercised deterministically instead of only waiting for real combat damage.
        // ShadowWheelsStillPresent's actual check is `info.m_bWheelPresent` on the vehicle's OWN
        // m_wheels[] entry, not whether the Wheel object itself still exists - confirmed via
        // disassembly of WheelRuntimeInfo::SetWheel (VA 0x5CE8F0): it sets m_wheel=arg and
        // m_bWheelPresent=(arg!=nullptr) in one place. A raw DetachFromPhysicObj() call on the
        // Wheel object alone does NOT flip this flag (tried first, live - no rebuild triggered),
        // so SetWheel(nullptr) is the actual, precise way to simulate what real wheel loss does
        // to this specific piece of state.
        const float tearAtT = kraken::Config::Instance().testharness_tear_wheel_at_t.value;
        if (!state.tornWheel && tearAtT >= 0.0f && state.clock >= tearAtT) {
            state.tornWheel = true;
            if (!vehicle->m_wheels.empty()) {
                LOG_INFO("Debug: tearing wheel 0 off target vehicle at t=%.3f", state.clock);
                vehicle->m_wheels[0].SetWheel(nullptr);
            } else {
                LOG_WARNING("Debug: tear_wheel_at_t fired but vehicle has no wheels");
            }
        }

        const Sample* sample = SampleAt(state, state.clock);
        if (sample) {
            // SetThrottle (not a raw m_throttle write) matters here: it also clears
            // m_bAutoBrake, which _KeepThrottle otherwise reads every tick to decide
            // whether to auto-re-engage the handbrake and zero the throttle behind our
            // back. A raw field write leaves m_bAutoBrake at whatever the save had it
            // (observed true), silently cancelling every scripted throttle input.
            vehicle->SetThrottle(sample->throttle, false);
            vehicle->m_steerRadians = sample->steer;
            vehicle->SetBrake(sample->brake);
            vehicle->m_bHandBrake   = sample->handbrake;
        }

        hta::CVector    pos    = vehicle->GetPosition();
        hta::Quaternion rot    = vehicle->GetRotation();
        hta::CVector    com    = vehicle->GetMassCenterPosition();
        hta::CVector    linVel = vehicle->GetLinearVelocity();
        hta::CVector    angVel = vehicle->GetAngularVelocity();

        uint32_t numWheels          = vehicle->GetNumWheels();
        int32_t  drivenWheels       = 0;
        int32_t  drivenWheelsJointed = 0;
        for (uint32_t i = 0; i < numWheels; ++i) {
            const hta::ai::Wheel* wheel = vehicle->GetWheel(i);
            if (!wheel) continue;
            if (wheel->m_driven) {
                drivenWheels++;
                if (wheel->m_jointID) drivenWheelsJointed++;
            }
        }

        state.telemetry
            << state.clock  << ',' << pos.x    << ',' << pos.y    << ',' << pos.z    << ','
            << rot.x        << ',' << rot.y    << ',' << rot.z    << ',' << rot.w    << ','
            << com.x        << ',' << com.y    << ',' << com.z    << ','
            << linVel.x     << ',' << linVel.y << ',' << linVel.z << ','
            << angVel.x     << ',' << angVel.y << ',' << angVel.z << ','
            << vehicle->m_throttle << ',' << vehicle->m_steerRadians << ','
            << vehicle->m_brake    << ',' << (vehicle->m_bHandBrake ? 1 : 0) << ','
            << vehicle->m_currentGear << ',' << vehicle->GetEngineRpm() << ','
            << vehicle->m_realThrottle << ',' << vehicle->m_numWheelsTouchingGround << ','
            << numWheels << ',' << drivenWheels << ',' << drivenWheelsJointed << '\n';
        state.telemetry.flush();

        state.clock += dt;
        if (!state.samples.empty() && state.clock > state.samples.back().t + 0.5f) {
            FinishScenario("ok");
        }
    }

    using CollideSceneFn = void(__fastcall*)(void*, float);
    static CollideSceneFn Real_CollideScene = reinterpret_cast<CollideSceneFn>(0x00603150);

    static void __fastcall CollideSceneHook(void* unused, float elapsedTime) {
        Real_CollideScene(unused, elapsedTime);
        GodModeTick();
        PerfTick();
        Tick(elapsedTime);
    }

    // ai::Vehicle::_KeepThrottle (0x5DAAE0) is the ONLY place that consumes
    // m_throttle/m_brake/m_bHandBrake into m_realThrottle (which _KeepGearBox then
    // turns into actual wheel torque). Writing those fields from Tick() above is not
    // enough: something else (fix::cardan's own reimplementation at this same call
    // site, and/or live input-device polling for whatever controller is plugged in)
    // also writes them every tick and reliably wins the race, observed as
    // m_throttle==0 here even while Tick() had just written 1.0 moments earlier.
    // Fastcall-with-a-dummy-EDX-param is the same trick fix::cardan itself uses to
    // hook a thiscall call site (this->ECX, the stack-passed bool lines up either way).
    using KeepThrottleFn = void(__fastcall*)(hta::ai::Vehicle*, void*, bool);
    static KeepThrottleFn Real_KeepThrottle = reinterpret_cast<KeepThrottleFn>(0x005DAAE0);

    static void __fastcall KeepThrottleHook(hta::ai::Vehicle* vehicle, void*, bool applyActions) {
        State& state = g_state;
        if (state.running && vehicle == GetTargetVehicle()) {
            const Sample* sample = SampleAt(state, state.clock);
            if (sample) {
                vehicle->m_throttle   = sample->throttle;
                vehicle->m_brake      = sample->brake;
                vehicle->m_bHandBrake = sample->handbrake;
                vehicle->m_bAutoBrake = false;
            }
        }
        Real_KeepThrottle(vehicle, nullptr, applyActions);
    }

    // Bootstrap-only: CollideScene (and therefore the whole harness above) does not run
    // until a save is already loaded and a vehicle exists, so it cannot load the first
    // save itself. ProcessAllEvents runs every loop iteration from the moment WinMain
    // starts the game, including at the main menu with nothing loaded yet - the one place
    // we can auto-trigger a save load with no human input. Opt-in (autoload_save) and
    // fires at most once per process lifetime.
    //
    // Profile folder names are arbitrary (often non-ASCII, e.g. Cyrillic) and differ
    // per install, so instead of hardcoding one, pick the most-recently-written save
    // across all profiles under data/profiles/*/saves/*. std::filesystem::path::string()
    // converts via the ANSI codepage on Windows, matching what hta::CStr/GetPrivateProfileStringA
    // expect, so this never has to construct a non-ASCII literal in source.
    static std::string FindMostRecentSaveDir() {
        fs::path best;
        fs::file_time_type bestTime{};
        bool found = false;

        std::error_code ec;
        for (const auto& profileEntry : fs::directory_iterator("data\\profiles", ec)) {
            if (ec || !profileEntry.is_directory()) continue;

            std::error_code savesEc;
            for (const auto& saveEntry : fs::directory_iterator(profileEntry.path() / "saves", savesEc)) {
                if (savesEc || !saveEntry.is_directory()) continue;

                std::error_code timeEc;
                fs::file_time_type t = fs::last_write_time(saveEntry.path(), timeEc);
                if (timeEc) continue;

                if (!found || t > bestTime) {
                    best = saveEntry.path();
                    bestTime = t;
                    found = true;
                }
            }
        }

        return found ? best.string() : std::string();
    }

    // Despite the PDB labeling CMiracle3d::LoadSavedGame as __thiscall(this@ecx), the
    // actual prologue at 0x4202c0 is `mov edi, eax` - it reads "this" from EAX, not ECX
    // (confirmed: calling it through a normal __thiscall function pointer leaked the
    // function pointer's OWN address as "this", since MSVC materializes an indirect
    // call target into EAX right before `call eax`, and edi/"this" was never
    // dereferenced until deep inside LoadMap - which is why it crashed several calls
    // later instead of immediately). MSVC's whole-program optimizer can pick whatever
    // register suits a function's actual call sites once it isn't external ABI, so this
    // is a real __usercall, not the thiscall the PDB defaults to. Call it by hand.
    static bool CallLoadSavedGame(void* self, hta::CStr* saveDir) {
        bool result = false;
        __asm {
            mov eax, self
            mov ecx, saveDir
            push ecx
            mov edx, 0x004202C0
            call edx
            mov result, al
        }
        return result;
    }

    static void TryAutoLoadSave(void* self) {
        std::string saveDirPath = FindMostRecentSaveDir();
        if (saveDirPath.empty()) {
            LOG_ERROR("Auto-load: no save found under data/profiles/*/saves/*, skipping");
            return;
        }

        // LoadSavedGame checks <saveDir>\currentmap.xml, which on disk lives under a
        // "maps" subfolder of the save (saves/00000032/maps/currentmap.xml), not the
        // save root itself.
        saveDirPath += "\\maps";

        void* vtable = *reinterpret_cast<void**>(self);
        LOG_INFO("Auto-load: CMiracle3d this=%p vtable=%p, loading '%s'", self, vtable, saveDirPath.c_str());

        hta::CStr saveDir(saveDirPath.c_str());
        bool ok = CallLoadSavedGame(self, &saveDir);
        LOG_INFO("Auto-load: LoadSavedGame returned %d", ok ? 1 : 0);
    }

    using ProcessAllEventsFn = void(__thiscall*)(void*);
    static ProcessAllEventsFn Real_ProcessAllEvents = reinterpret_cast<ProcessAllEventsFn>(0x005A7BC0);
    static bool g_autoloadAttempted = false;
    static int  g_autoloadFrameCounter = 0;

    // ProcessAllEvents is called once per m3d::Application::run() loop iteration,
    // BEFORE that same iteration's OneFrame() - which is what actually loads the main
    // menu level. Firing on the very first call means no level has ever been loaded yet,
    // and CMiracle3d::LoadMap -> CleanLevel -> DeleteAllFilesInDirectory hits
    // `assert(strDir.length() > 0)` on whatever "previous map temp dir" state only a
    // real level load would populate. Give the normal boot flow a few hundred frames to
    // reach the main menu on its own first.
    static const int AUTOLOAD_DELAY_FRAMES = 300;

    // Sole pointer parameter -> __fastcall and __thiscall place it identically (ecx),
    // and MSVC disallows __thiscall on a free function definition (C3865).
    static void __fastcall ProcessAllEventsHook(void* self) {
        Real_ProcessAllEvents(self);
        if (!g_autoloadAttempted) {
            g_autoloadFrameCounter++;
            if (g_autoloadFrameCounter >= AUTOLOAD_DELAY_FRAMES) {
                g_autoloadAttempted = true;
                TryAutoLoadSave(self);
            }
        }
    }

    void Apply() {
        const kraken::Config& config = kraken::Config::Instance();
        if (config.testharness.value == 0) return;

        LOG_INFO("Feature enabled");

        g_state.baseDir = fs::path("./data/kraken_testharness");
        std::error_code ec;
        fs::create_directories(g_state.baseDir, ec);
        if (ec) {
            LOG_ERROR("Failed to create kraken_testharness directory");
            return;
        }

        routines::ChangeCall((void*) 0x005F438D, &CollideSceneHook);

        // Takes over fix::cardan's own patch of this same call site (whichever Apply()
        // runs last wins outright - ChangeCall doesn't chain). Acceptable here: testharness
        // is an opt-in automated-testing mode, not meant to run during normal play, so
        // losing cardan's cosmetic chassis-animation fix while it's active is a non-issue.
        routines::ChangeCall((void*) 0x005EC7AD, &KeepThrottleHook);

        if (config.testharness_autoload.value != 0) {
            LOG_INFO("Auto-load enabled, will load the most recent save once ProcessAllEvents first runs");
            routines::ChangeCall((void*) 0x005A7FFF, &ProcessAllEventsHook);
        }

        if (config.testharness_god_mode.value != 0) {
            g_godMode.enabled = true;
            LOG_INFO("God mode enabled - will apply setGodMode/setImmortalMode(true) to the player vehicle as soon as it exists (bypasses cheats.lua's testcheat() computer-name gate)");
        }

        if (config.testharness_perfmon.value != 0) {
            g_perf.enabled     = true;
            g_perf.intervalSec = (double) config.testharness_perfmon_interval.value;
            LOG_INFO("Perf monitor enabled - logging wall-clock fps/frame-time every %.1fs", g_perf.intervalSec);
        }
    }
}
