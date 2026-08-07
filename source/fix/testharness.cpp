#define LOGGER "testharness"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "ext/logger.hpp"
#include "routines.hpp"

#include "hta/CVector.hpp"
#include "hta/Quaternion.hpp"
#include "hta/ai/DynamicScene.hpp"
#include "hta/ai/Vehicle.hpp"
#include "hta/ai/Wheel.hpp"

#include "fix/testharness.hpp"

namespace kraken::fix::testharness {
    namespace fs = std::filesystem;

    // M0 deliberately keeps these options local.  Config on multiplayer/main predates
    // the Jolt testharness fields, while the harness must remain cherry-pickable without
    // changing config.cpp/config.hpp.  The feature is opt-in: KRAKEN_TESTHARNESS_ENABLED=1
    // is required in each process.  This is important because KeepThrottleHook shares a call
    // site with fix::cardan.
    struct TestHarnessOptions {
        bool        enabled    = false;
        std::string instanceTag = "default";
        fs::path    rootDir     = fs::path("./data/kraken_testharness");
    };

    struct Sample {
        float t        = 0.0f;
        float throttle = 0.0f;
        float steer    = 0.0f;
        float brake    = 0.0f;
        bool  handbrake = false;
    };

    struct State {
        TestHarnessOptions options;
        std::string         lastTrigger;
        std::string         token;
        std::vector<Sample> samples;
        hta::CVector         spawnPos;
        hta::Quaternion      spawnRot;
        bool                 hasSpawn = false;
        bool                 running  = false;
        float                clock    = 0.0f;
        std::ofstream        telemetry;
    };

    static State g_state;
    static bool g_applied = false;

    static std::string EnvironmentValue(const char* name) {
        char* value = nullptr;
        size_t length = 0;
        if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) return {};

        std::string result(value, length > 0 ? length - 1 : 0);
        std::free(value);
        return result;
    }

    bool IsEnabled() {
        return EnvironmentValue("KRAKEN_TESTHARNESS_ENABLED") == "1";
    }

    static std::string SafeTag(std::string value) {
        if (value.empty()) value = "default";
        for (char& c : value) {
            const bool safe =
                (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_' || c == '-';
            if (!safe) c = '_';
        }
        if (value.size() > 48) value.resize(48);
        return value.empty() ? "default" : value;
    }

    static TestHarnessOptions ReadOptions() {
        TestHarnessOptions options;

        const std::string enabled = EnvironmentValue("KRAKEN_TESTHARNESS_ENABLED");
        options.enabled = enabled == "1";

        std::string tag = EnvironmentValue("KRAKEN_INSTANCE_TAG");
        if (tag.empty()) tag = EnvironmentValue("KRAKEN_TESTHARNESS_TAG");
        options.instanceTag = SafeTag(tag);
        options.rootDir /= options.instanceTag;
        return options;
    }

    // The original Jolt harness called its shadow-teleport hook after writing the
    // ODE-side Vehicle position.  Multiplayer/main has no shadow body.  ODE remains the
    // authoritative body here, so this compatibility seam is intentionally a no-op: adding
    // a second physics representation would make a scripted spawn non-deterministic and would
    // violate the branch's PhysicObj-only boundary.
    static void TeleportPlayerShadow(const hta::CVector&, const hta::Quaternion&) {
        // ODE-compatible no-op. Vehicle::SetPositionSelf/SetRotationSelf did the teleport.
    }

    static std::vector<std::string> Split(const std::string& line) {
        std::vector<std::string> tokens;
        std::stringstream stream(line);
        std::string token;
        while (std::getline(stream, token, ',')) tokens.push_back(token);
        return tokens;
    }

    static bool ParseSample(const std::vector<std::string>& tokens, Sample& sample) {
        if (tokens.size() < 5 || tokens[0] == "t") return false;
        try {
            sample.t         = std::stof(tokens[0]);
            sample.throttle  = std::stof(tokens[1]);
            sample.steer     = std::stof(tokens[2]);
            sample.brake     = std::stof(tokens[3]);
            sample.handbrake = std::stoi(tokens[4]) != 0;
        } catch (const std::exception&) {
            return false;
        }
        return true;
    }

    static bool LoadScenario(const fs::path& path, State& state) {
        std::ifstream input(path);
        if (!input.is_open()) {
            LOG_ERROR("instance=%s: cannot open scenario '%s'", state.options.instanceTag.c_str(), path.string().c_str());
            return false;
        }

        state.samples.clear();
        state.hasSpawn = false;

        std::string line;
        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;

            const std::vector<std::string> tokens = Split(line);
            if (tokens.empty()) continue;

            if (tokens[0] == "spawn" && tokens.size() >= 8) {
                try {
                    state.spawnPos = hta::CVector(
                        std::stof(tokens[1]), std::stof(tokens[2]), std::stof(tokens[3]));
                    state.spawnRot = hta::Quaternion(
                        std::stof(tokens[4]), std::stof(tokens[5]),
                        std::stof(tokens[6]), std::stof(tokens[7]));
                    state.hasSpawn = true;
                } catch (const std::exception&) {
                    LOG_WARNING("instance=%s: invalid spawn row ignored", state.options.instanceTag.c_str());
                }
                continue;
            }

            Sample sample;
            if (ParseSample(tokens, sample)) state.samples.push_back(sample);
        }

        std::sort(state.samples.begin(), state.samples.end(), [](const Sample& lhs, const Sample& rhs) {
            return lhs.t < rhs.t;
        });
        return !state.samples.empty();
    }

    static const Sample* SampleAt(const State& state, float time) {
        const Sample* result = nullptr;
        for (const Sample& sample : state.samples) {
            if (sample.t <= time) result = &sample;
            else break;
        }
        if (result == nullptr && !state.samples.empty()) result = &state.samples.front();
        return result;
    }

    static hta::ai::Vehicle* GetTargetVehicle() {
        hta::ai::DynamicScene* scene = hta::ai::DynamicScene::Instance();
        return scene != nullptr ? scene->GetVehicleControlledByPlayer() : nullptr;
    }

    static std::string SafeFileToken(std::string value) {
        if (value.empty()) value = "scenario";
        for (char& c : value) {
            const bool safe =
                (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_' || c == '-';
            if (!safe) c = '_';
        }
        if (value.size() > 80) value.resize(80);
        return value;
    }

    static void FinishScenario(const char* reason) {
        State& state = g_state;
        hta::ai::Vehicle* vehicle = GetTargetVehicle();
        if (vehicle != nullptr) {
            vehicle->SetThrottle(0.0f, false);
            vehicle->m_steerRadians = 0.0f;
            vehicle->SetBrake(1.0f);
            vehicle->m_bHandBrake = true;
        }

        if (state.telemetry.is_open()) state.telemetry.close();
        std::ofstream done(state.options.rootDir / ("output_" + state.token + ".done"), std::ios::trunc);
        done << reason << '\n';
        state.running = false;

        LOG_INFO("instance=%s: scenario token=%s finished reason=%s",
            state.options.instanceTag.c_str(), state.token.c_str(), reason);
    }

    static void StartScenario(const std::string& rawToken) {
        State& state = g_state;
        if (!LoadScenario(state.options.rootDir / "scenario.csv", state)) {
            LOG_ERROR("instance=%s: scenario trigger ignored because scenario.csv is invalid",
                state.options.instanceTag.c_str());
            return;
        }

        state.token   = SafeFileToken(rawToken);
        state.clock   = 0.0f;
        state.running = true;

        hta::ai::Vehicle* vehicle = GetTargetVehicle();
        if (vehicle != nullptr) {
            if (state.hasSpawn) {
                vehicle->SetPositionSelf(state.spawnPos);
                vehicle->SetRotationSelf(state.spawnRot);
                // Keep the original semantic seam, but do not touch a non-existent Jolt body.
                TeleportPlayerShadow(state.spawnPos, state.spawnRot);
            }
            vehicle->SetLinearVelocity(hta::CVector(0.0f, 0.0f, 0.0f));
            vehicle->SetAngularVelocity(hta::CVector(0.0f, 0.0f, 0.0f));
            vehicle->SetThrottle(0.0f, false);
            vehicle->m_steerRadians = 0.0f;
            vehicle->SetBrake(0.0f);
            vehicle->m_bHandBrake = false;
        }

        state.telemetry.open(
            state.options.rootDir / ("output_" + state.token + ".csv"), std::ios::trunc);
        state.telemetry
            << "t,px,py,pz,qx,qy,qz,qw,comx,comy,comz,vx,vy,vz,avx,avy,avz,"
               "throttle,steer,brake,handbrake,gear,engineRpm,realThrottle,"
               "wheelsTouchingGround,numWheels,drivenWheels,drivenWheelsJointed\n";

        LOG_INFO("instance=%s: scenario token=%s started samples=%u spawn=%d",
            state.options.instanceTag.c_str(), state.token.c_str(),
            static_cast<unsigned>(state.samples.size()), state.hasSpawn ? 1 : 0);
    }

    static void Tick(float dt) {
        State& state = g_state;

        std::ifstream trigger(state.options.rootDir / "trigger.txt");
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
        if (vehicle == nullptr) {
            FinishScenario("no_vehicle");
            return;
        }

        const Sample* sample = SampleAt(state, state.clock);
        if (sample != nullptr) {
            // SetThrottle also clears the game's auto-brake latch.  KeepThrottleHook below
            // repeats the values at the engine's consumption point so controller polling cannot
            // win the race over deterministic scripted input.
            vehicle->SetThrottle(sample->throttle, false);
            vehicle->m_steerRadians = sample->steer;
            vehicle->SetBrake(sample->brake);
            vehicle->m_bHandBrake = sample->handbrake;

            const uint32_t wheelCount = vehicle->GetNumWheels();
            for (uint32_t index = 0; index < wheelCount; ++index) {
                hta::ai::Vehicle::WheelRuntimeInfo& info = vehicle->m_wheels[index];
                if (!info.m_bWheelPresent || info.m_wheel == nullptr) continue;

                hta::ai::Wheel* wheel = info.m_wheel;
                float sign = 0.0f;
                if (wheel->m_steering == hta::ai::Wheel::STEERING_CORRECT) sign = 1.0f;
                else if (wheel->m_steering == hta::ai::Wheel::STEERING_INVERSE) sign = -1.0f;
                if (sign != 0.0f) wheel->m_curAngle = sign * sample->steer;
            }
        }

        const hta::CVector pos    = vehicle->GetPosition();
        const hta::Quaternion rot = vehicle->GetRotation();
        const hta::CVector com    = vehicle->GetMassCenterPosition();
        const hta::CVector linVel = vehicle->GetLinearVelocity();
        const hta::CVector angVel = vehicle->GetAngularVelocity();

        const uint32_t numWheels = vehicle->GetNumWheels();
        int32_t drivenWheels = 0;
        int32_t drivenWheelsJointed = 0;
        for (uint32_t index = 0; index < numWheels; ++index) {
            const hta::ai::Wheel* wheel = vehicle->GetWheel(index);
            if (wheel == nullptr) continue;
            if (wheel->m_driven) {
                ++drivenWheels;
                if (wheel->m_jointID) ++drivenWheelsJointed;
            }
        }

        state.telemetry
            << state.clock << ',' << pos.x << ',' << pos.y << ',' << pos.z << ','
            << rot.x << ',' << rot.y << ',' << rot.z << ',' << rot.w << ','
            << com.x << ',' << com.y << ',' << com.z << ','
            << linVel.x << ',' << linVel.y << ',' << linVel.z << ','
            << angVel.x << ',' << angVel.y << ',' << angVel.z << ','
            << vehicle->m_throttle << ',' << vehicle->m_steerRadians << ','
            << vehicle->m_brake << ',' << (vehicle->m_bHandBrake ? 1 : 0) << ','
            << vehicle->m_currentGear << ',' << vehicle->GetEngineRpm() << ','
            << vehicle->m_realThrottle << ',' << vehicle->m_numWheelsTouchingGround << ','
            << numWheels << ',' << drivenWheels << ',' << drivenWheelsJointed << '\n';
        state.telemetry.flush();

        state.clock += dt;
        if (!state.samples.empty() && state.clock > state.samples.back().t + 0.5f) {
            FinishScenario("ok");
        }
    }

    // The patched call is a native __thiscall site: ECX carries self and the float is pushed
    // on the stack.  A free-function __fastcall wrapper therefore needs the dummy EDX slot;
    // without it elapsedTime would be read from EDX instead of from the stack.
    using CollideSceneFn = void(__fastcall*)(void*, void*, float);
    static CollideSceneFn Real_CollideScene = reinterpret_cast<CollideSceneFn>(0x00603150);

    static void __fastcall CollideSceneHook(void* self, void*, float elapsedTime) {
        Real_CollideScene(self, nullptr, elapsedTime);
        Tick(elapsedTime);
    }

    // _KeepThrottle is where the game consumes m_throttle/m_brake/m_bHandBrake into
    // m_realThrottle.  Writing the fields from Tick alone is racy with live input polling;
    // this small hook makes playback deterministic while a scenario is active.
    using KeepThrottleFn = void(__fastcall*)(hta::ai::Vehicle*, void*, bool);
    static KeepThrottleFn Real_KeepThrottle = reinterpret_cast<KeepThrottleFn>(0x005DAAE0);

    static void __fastcall KeepThrottleHook(
        hta::ai::Vehicle* vehicle, void*, bool applyActions) {
        State& state = g_state;
        if (state.running && vehicle == GetTargetVehicle()) {
            const Sample* sample = SampleAt(state, state.clock);
            if (sample != nullptr) {
                vehicle->m_throttle   = sample->throttle;
                vehicle->m_brake      = sample->brake;
                vehicle->m_bHandBrake = sample->handbrake;
                vehicle->m_bAutoBrake = false;
            }
        }
        Real_KeepThrottle(vehicle, nullptr, applyActions);
    }

    void Apply() {
        if (g_applied) return;
        g_applied = true;

        g_state.options = ReadOptions();
        if (!g_state.options.enabled) {
            LOG_INFO("instance=%s: disabled; set KRAKEN_TESTHARNESS_ENABLED=1 to enable",
                g_state.options.instanceTag.c_str());
            return;
        }

        std::error_code error;
        fs::create_directories(g_state.options.rootDir, error);
        if (error) {
            LOG_ERROR("instance=%s: failed to create harness directory '%s'",
                g_state.options.instanceTag.c_str(), g_state.options.rootDir.string().c_str());
            return;
        }

        try {
            routines::ChangeCall(reinterpret_cast<void*>(0x005F438D), &CollideSceneHook);
            routines::ChangeCall(reinterpret_cast<void*>(0x005EC7AD), &KeepThrottleHook);
        }
        catch (const std::exception& exception) {
            LOG_ERROR("instance=%s: failed to install hooks: %s",
                g_state.options.instanceTag.c_str(), exception.what());
            return;
        }

        LOG_INFO("instance=%s: enabled root=%s (scenario.csv + trigger.txt)",
            g_state.options.instanceTag.c_str(), g_state.options.rootDir.string().c_str());
    }
}
