#define LOGGER "cinematic"

#include "ext/logger.hpp"
#include "routines.hpp"

#include "hta/m3d/Application.hpp"
#include "hta/m3d/Cinematic.hpp"
#include "hta/CMiracle3d.hpp"
#include "hta/CinemaPanel.hpp"
#include "hta/m3d/Enums.hpp"
#include "hta/ai/CServer.hpp"
#include "hta/ai/ProcessManager.hpp"
#include "hta/ai/Submarine.hpp"

#include "fix/cinematic.hpp"
#include "hta/ai/Player.hpp"
#include "hta/CinemaFadePanel.hpp"

void HandleCinematic_Hook();

namespace kraken::fix::cinematic
{
    bool enable_fix{false};

    // anon namespace in vanilla
    hta::CinemaPanel* GetCinemaPanel()
    {
        const auto& app = hta::CMiracle3d::Instance();
        auto wnd = app->m_pInterfaceManager->GetWindow(18);
        if (!wnd)
        {
            return nullptr;
        }
        if (!wnd->IsKindOf((hta::m3d::Class*)0x00A07FBC)) // hta::CinemaPanel
        {
            return nullptr;
        }
        return reinterpret_cast<hta::CinemaPanel*>(&*wnd);
    }
    
    // CMiracle3d class method
    CUSTOM bool CinematicFade(hta::CMiracle3d* self) // custom reimplementation
    {
        auto &m_cinematic = self->m_cinematic;
        auto &m_pInterfaceManager = self->m_pInterfaceManager;

        int32_t fadeTime = m_cinematic->m_playTime - m_cinematic->m_fadeStartTime;
        int32_t fadePeriod = (int32_t)(m_cinematic->GetFadePeriodForState(m_cinematic->m_state) * 1000.0);

        bool result{true};

        // Skipped cinematic - make fade instant
        if (m_cinematic->m_bWasSkippedInEnterFadeOut)
        {
            auto &state = m_cinematic->m_state;
            if (state == hta::m3d::CinematicState::CINEMATIC_ENTER_FADE_IN || state == hta::m3d::CinematicState::CINEMATIC_EXIT_FADE_OUT)
            {
                fadeTime = fadePeriod;
            }
        }

        auto panel = GetCinemaPanel();

        // Fade still in progress
        if (fadeTime < fadePeriod)
        {
            // l_modalsJustClosed, global val in anonymous namespace
            static bool& l_modalsJustClosed = *reinterpret_cast<bool*>(0x00A41D20);

            if (l_modalsJustClosed)
                l_modalsJustClosed = false;
            return true;
        }

        const auto &app = hta::CMiracle3d::Instance();
        // Fade complete - handle state transitions
        switch (m_cinematic->m_state)
        {
        case hta::m3d::CinematicState::CINEMATIC_ENTER_FADE_OUT:
            {
            app->m_bGuiWasHiddenBeforeCinematic = app->m_pInterfaceManager->IsHiddenByUser();

            auto wndMainMenu = app->m_pInterfaceManager->GetWindow(72);

            bool isMainMenuChild = wndMainMenu->IsChildOf(reinterpret_cast<hta::m3d::ui::WndStation*>(&*app));

            // Hide GUI and close modals if needed
            if (!wndMainMenu.m_ptr || !isMainMenuChild)
            {
                app->m_pInterfaceManager->Show(false, true);

                // m3d::ui::WndStation *station = reinterpret_cast<m3d::ui::WndStation*>(&*m_cinematic)->GetStation();
                app->CloseAllModalWithCancelRet();
                static bool& l_modalsJustClosed = *reinterpret_cast<bool*>(0x00A41D20);
                l_modalsJustClosed = true;
            }

            // Clear cinema panel
            if (panel)
                panel->Clear();

            // Old: Show cinema panel window based on bit #2 in flag bit map
            // if ((m_cinematic->m_curItem.m_flags & 0x04) == 0)

            // New: Show cinema panel window based on bit #4 flag
            bool showCinemaPan  = (m_cinematic->m_curItem.m_flags >> 4) & 1;
            if (showCinemaPan)
            {
                app->m_pInterfaceManager->ShowWindow(18, true, true, false, false, nullptr);
            }

            // Transition to fade in state
            m_cinematic->m_fadeStartTime = m_cinematic->m_playTime;
            m_cinematic->m_state = hta::m3d::CinematicState::CINEMATIC_ENTER_FADE_IN;
            hta::ai::CServer::Instance()->PostPlayerEvent(hta::ai::eGameEvent::GE_CINEMATIC_ENTER_FADE_IN);

            // Old: Show fade panel if start faded are enabled (bit #0)
            // New: Check bit at index 1, which is start fade in
            if ((m_cinematic->m_curItem.m_flags >> 1) & 1)
            {
                app->m_pInterfaceManager->ShowWindow(19, true, true, false, false, nullptr);

                auto cinemaFadePanel = hta::GetCinemaFadePanel();

                if (cinemaFadePanel)
                {
                    // FIXME: this also might work as just app->MoveChildToFirstPosition, test
                    // m3d::ui::WndStation *station = &cinematic->m3d::ui::WndStation;
                    auto station = reinterpret_cast<hta::m3d::ui::WndStation*>(&*app);
                    if (cinemaFadePanel->IsDirectChild(station))
                        station->MoveChildToFirstPosition(cinemaFadePanel);
                }
            }

            // TODO: Check if this is necessary, probably not?
            // if (wndMainMenu.m_ptr)
            // {
            //     --wndMainMenu.m_ptr->m_refCount;
            //     if (wndMainMenu.m_ptr->m_refCount <= 0)
            //     {
            //         ((void(__thiscall *)(m3d::ui::Wnd *, int))wndMainMenu.m_ptr->~m3d::ui::Wnd)(wndMainMenu.m_ptr, 1);
            //     }
            // }

            return false;
        }

        case hta::m3d::CinematicState::CINEMATIC_ENTER_FADE_IN:
        {
            if (m_cinematic->m_bWasSkippedInEnterFadeOut)
            {
                // Skip directly to exit fade
                m_cinematic->m_fadeStartTime = m_cinematic->m_playTime;
                m_cinematic->m_state = hta::m3d::CinematicState::CINEMATIC_EXIT_FADE_OUT;
                return false;
            }
            else
            {
                // Start playing
                m_cinematic->m_state = hta::m3d::CinematicState::CINEMATIC_IS_PLAYING;
                app->m_pInterfaceManager->ShowWindow(19, false, false, false, false, nullptr);
                return true;
            }
        }

        case hta::m3d::CinematicState::CINEMATIC_EXIT_FADE_OUT:
        {
            m_cinematic->Stop();

            // TODO: deprecated, fog of war is irelevant for HTA, legacy out of strategy games
            // Re-enable fog of war (deprecated code kept for compatibility)
            // auto &engineCfg = hta::m3d::Kernel::Instance()->GetEngineCfg();
            // engineCfg.m_FogOfWar.SetI(1, false);

            hta::ai::CServer::Instance()->EndCinematic();

            // Handle cinematic continuation or ending
            if (m_cinematic->m_bWasSkipped)
            {
                // Skip remaining cinematics
                hta::ai::CServer::Instance()->PostPlayerEvent(hta::ai::eGameEvent::GE_SKIP_CINEMATIC);
                while (m_cinematic->SkipCinematic())
                {
                    // do nothing
                }
                m_cinematic->m_bWasSkipped = false;
            }
            else if (m_cinematic->m_cinematicItems.size() > 0)
            {
                // Start next cinematic fly path
                auto player = hta::ai::Player::Instance();
                if (player)
                {
                    hta::CStr flyPathName = m_cinematic->GetNextFlyPathName();
                    hta::m3d::AIParam aiParam(flyPathName);
                    player->CauseEvent(hta::ai::eGameEvent::GE_START_CINEMATIC_FLY, 0.0, aiParam, hta::m3d::AIParam());
                }
                else
                {
                    hta::ai::CServer::Instance()->PostPlayerEvent(hta::ai::eGameEvent::GE_SKIP_CINEMATIC);
                    while (m_cinematic->SkipCinematic())
                    {
                        // do nothing
                    }
                }
            }
            else
            {
                // All cinematics done
                hta::ai::CServer::Instance()->PostPlayerEvent(hta::ai::eGameEvent::GE_END_CINEMATIC);
            }

            m_cinematic->m_fadeStartTime = m_cinematic->m_playTime;
            m_cinematic->m_state = hta::m3d::CinematicState::CINEMATIC_EXIT_FADE_IN;

            // Check if there's another cinematic in queue
            if (m_cinematic->bMustBeNextCinematic())
            {
                if (panel)
                {
                    panel->Clear();
                }
                return false;
            }
            else
            {
                // End of all cinematics - cleanup
                if (panel)
                    panel->OnHide();

                // Restore GUI state
                bool shouldShowGui = !app->m_bGuiWasHiddenBeforeCinematic;
                app->m_pInterfaceManager->Show(shouldShowGui, true);

                // Restore game mode
                hta::m3d::AuxImpulseInfo impulseInfo(3, 1, app->m_curGameMode.m_mode, 1u, false);
                app->OnChangeMode(impulseInfo);

                return false;
            }
        }

        case hta::m3d::CinematicState::CINEMATIC_EXIT_FADE_IN:
        {
            // Hide fade panel
            app->m_pInterfaceManager->ShowWindow(19, false, false, false, false, nullptr);

            // Either start next cinematic or finish
            if (m_cinematic->bMustBeNextCinematic())
            {
                m_cinematic->StartCinematic();
            }
            else
            {
                m_cinematic->LoadDefaults();
                // FIXME: same question as above, what is the CORRECT way to do this, first cast then call or just call on app?
                //auto station = reinterpret_cast<hta::m3d::ui::WndStation*>(&*app); 
                //station->SetCursorShow(true); // TODO: or app->SetCursorShow(true) ?
                app->SetCursorShow(true);
            }

            return true;
        }

        default:
            return true;
        }

    };

    int32_t __fastcall HandleCinematic_Impl(hta::CMiracle3d* self, void* unused_edx, float dT)
    {
        volatile float dt_check = dT; // Force the compiler to read it from stack

        if (self->m_cinematic->bCanUpdate())
        {
            self->m_cinematic->Update(self->m_curCamera, dT);
        }
        self->m_cinematic->m_playTime += dT * 1000;

        while(true)
        {
            switch (self->m_cinematic->m_state)
            {
            case hta::m3d::CinematicState::CINEMATIC_ENTER_FADE_OUT:
            case hta::m3d::CinematicState::CINEMATIC_ENTER_FADE_IN:
            case hta::m3d::CinematicState::CINEMATIC_EXIT_FADE_OUT:
            case hta::m3d::CinematicState::CINEMATIC_EXIT_FADE_IN:
            {
                if (!CinematicFade(self))
                    continue;
                return 1;
            }
            case hta::m3d::CinematicState::CINEMATIC_IS_PLAYING:
            {
                hta::ai::CServer::Instance()->PostPlayerEvent(hta::ai::eGameEvent::GE_IN_CINEMATIC);
                auto fadePeriod = self->m_cinematic->GetFadePeriodForState(hta::m3d::CinematicState::CINEMATIC_EXIT_FADE_OUT);

                auto timeToShowDlg = 0.0;
                if (!self->m_cinematic->InPlay() || self->m_cinematic->GetTimeToTheEnd() >= 0.0)
                {
                    timeToShowDlg = self->m_cinematic->GetTimeToTheEnd();
                }
                auto cinemaPanel = GetCinemaPanel();
                if (self->m_cinematic->bWaitWhenStop() || fadePeriod < timeToShowDlg || cinemaPanel && (cinemaPanel->HasMsg()))
                {
                    return 1;
                }

                self->CinematicInterrupt();
                break;
            }
            case hta::m3d::CinematicState::CINEMATIC_NOT_INITED:
                return 0;

            default:
                return 1;
            }
        }
        return 0;
    }

    // anon namespace in vanilla
    int32_t __fastcall n_SetCinematicCinemaPanel(hta::m3d::sArgStack& scriptStack, void* _)
    {
        if (scriptStack.getNumInArgs() != 1)
            return -1;

        hta::m3d::sArg *arg = scriptStack.popIn();
        bool value{};
        switch (arg->m_type) {
            case hta::m3d::sArg::eArgType::ARGTYPE_BOOL:
                value = arg->GetB();
                break;

            case hta::m3d::sArg::eArgType::ARGTYPE_FLOAT:
                value = arg->GetF() != 0.0;
                break;

            case hta::m3d::sArg::eArgType::ARGTYPE_INT:
                value = arg->GetI() != 0;
                break;

            default:
                return -1;
        }

        hta::m3d::Cinematic* cinematic = hta::m3d::Application::Instance()->m_cinematic;
        int flags = cinematic->GetFlags();

        if (value) {
            flags |= (1 << 4); // set flag at index 4
        } else {
            flags &= ~(1 << 4); // clear flag at index 4
        }
        cinematic->SetFlags(flags);
        return 1;

    }


    // anon namespace in vanilla
    int32_t __fastcall n_SetCinematicFadeParams(hta::m3d::sArgStack& scriptStack, void* _)
    {
        if (scriptStack.getNumInArgs() != 2)
            return -1;

        hta::m3d::Cinematic *cinematic = hta::m3d::Application::Instance()->m_cinematic;

        // Process first argument (start fades)
        hta::m3d::sArg *arg1 = scriptStack.popIn();
        if (arg1->m_type != hta::m3d::sArg::eArgType::ARGTYPE_FLOAT)
            return -1;

        int flags = cinematic->GetFlags();
        int start_mode = (int)arg1->GetF();
        // Would be super nice to have 3 as "both on", but for compatibility reasons this is better
        switch (start_mode)
        {
            case 0: // both fades off
                flags &= ~0b0011;
                break;
            case 1: // both on
                flags |= 0b0011;
                break;
            case 2: // out on, in off
                LOG_DEBUG("Extended start fade mode enabled: START_FADE_OUT_ONLY");
                flags |= 0b0001;
                break;
            case 3: // out off, in on
                LOG_DEBUG("Extended start fade mode enabled: START_FADE_IN_ONLY");
                flags |= 0b0010;
                break;
            default:
                LOG_WARNING("Tripped default in (start_mode) switch of n_SetCinematicFadeParams");
                flags |= 0b0011; // dup of 1 for debug
                break;
        }
        // cinematic->SetFlags(flags); // why double set?

        // Process second argument (end fades)
        hta::m3d::sArg *arg2 = scriptStack.popIn();
        if (arg2->m_type != hta::m3d::sArg::eArgType::ARGTYPE_FLOAT)
            return -1;

        // flags = cinematic->GetFlags(); // why double get?

        int end_mode = (int)arg2->GetF();
        switch (end_mode)
        {
            case 0: // both fades off
                flags &= ~0b1100;
                break;
            case 1: // both on
                flags |= 0b1100;
                break;
            case 2: // out on, in off
                LOG_DEBUG("Extended end fade mode enabled: END_FADE_OUT_ONLY");
                flags |= 0b0100;
                break;
            case 3: // out off, in on
                LOG_DEBUG("Extended end fade mode enabled: END_FADE_IN_ONLY");
                flags |= 0b1000;
                break;
            default:
                LOG_WARNING("Tripped default in (end_mode) switch of n_SetCinematicFadeParams");
                flags |= 0b1100; // dup of 1 for debug
                break;
        }
        cinematic->SetFlags(flags);

        return 1;
    }

    // Cinematic:: class method in vanilla, (REIMPL but some logic is custom, so dunno)
    float __fastcall GetFadePeriodForState(hta::m3d::Cinematic *cinematic, void* _, hta::m3d::CinematicState state)
    {
        // Extract fade mode from flags (bits #0-3), bit #4 will be used for cinemapanel visibility
        // 0[0b00] = no fade, 1[0b01] = fade in only, 2[0b10] = fade out only, 3[0b11] = both fades
        int32_t flags = cinematic->m_curItem.m_flags;
        bool enterFadeOut = (flags >> 0) & 1;
        bool enterFadeIn  = (flags >> 1) & 1;
        bool exitFadeOut  = (flags >> 2) & 1;
        bool exitFadeIn   = (flags >> 3) & 1;
        
        bool shouldFade{false};
        
        switch (state)
        {
            case hta::m3d::CinematicState::CINEMATIC_ENTER_FADE_OUT:
                shouldFade = cinematic->m_bWasSkipped || enterFadeOut;
                break;
            case hta::m3d::CinematicState::CINEMATIC_ENTER_FADE_IN:
                shouldFade = cinematic->m_bWasSkipped || enterFadeIn;
                break;
            case hta::m3d::CinematicState::CINEMATIC_EXIT_FADE_OUT:
                shouldFade = cinematic->m_bWasSkipped || exitFadeOut;
                break;
            case hta::m3d::CinematicState::CINEMATIC_EXIT_FADE_IN:
                shouldFade = cinematic->m_bWasSkipped || exitFadeIn;
                break;
            default:
                return 0.0;
        }
        
        if (!shouldFade)
            return 0.0;
        
        if (cinematic->m_fadePeriod.m_type == hta::m3d::CVar::eType::CVAR_FLOAT)
            return cinematic->m_fadePeriod.m_f;
        else
            return (float)cinematic->m_fadePeriod.m_i;
    }

    int32_t __fastcall CinematicClear(hta::CMiracle3d* dunno, void* _, hta::CMiracle3d* self)
    {
        hta::m3d::Cinematic* m_cinematic = self->m_cinematic;
        
        if (m_cinematic->m_state != hta::m3d::CinematicState::CINEMATIC_NOT_INITED)
        {
            m_cinematic->Stop();
            auto panel = GetCinemaPanel();
            if (panel)
                panel->OnHide();

            self->m_pInterfaceManager->ShowWindow(19, false, false, false, false, nullptr);
            self->m_pInterfaceManager->Show(!self->m_bGuiWasHiddenBeforeCinematic, true);

            auto station = self->GetStation();
            station->SetCursorShow(true);
            //hta::m3d::Kernel::Instance()->GetEngineCfg().m_FogOfWar.SetI(1, true); // m_FogOfWar is a legacy thing from older games

            // Post end cinematic event if fade out is enabled

            // Old: Fade out enabled if bit at index 1 is set
            //if (m_cinematic->m_curItem.m_flags & 0x02) {
            
            // New: separate bits for start and end fade outs 
            bool startFadeOut = (m_cinematic->m_curItem.m_flags >> 1) & 1;
            bool endFadeOut = (m_cinematic->m_curItem.m_flags >> 3) & 1;
            if (startFadeOut || endFadeOut)
            {
                hta::ai::CServer::Instance()->PostPlayerEvent(hta::ai::eGameEvent::GE_END_CINEMATIC);
            }
            m_cinematic->LoadDefaults();
        }
        return 1;

    }

    REIMPL int32_t __fastcall n_UpdateCinematic(hta::m3d::sArgStack& scriptStack, void* _)
    {
        if (scriptStack.getNumInArgs() != 1)
        {
            auto stackSize = scriptStack.getNumInArgs();
            for (int i = 0; i < stackSize; i++) {
                auto* arg = scriptStack.popIn();
                auto argT = arg->GetType();
            }
            return -1;
        }

        auto* arg = scriptStack.popIn();
        int32_t value{};
        if (arg->GetType() == hta::m3d::sArg::ARGTYPE_INT)
        {
            value = arg->GetI();
        }
        else if (arg->GetType() == hta::m3d::sArg::ARGTYPE_FLOAT)
        {
            value = static_cast<int>(arg->GetF());
        }
        HandleCinematic_Impl(hta::CMiracle3d::Instance(), nullptr, value);
        return 1;
    }

    // ai::Town class method
    void __fastcall _StartCinematic(hta::ai::Town* self, void* _, hta::ai::Vehicle* pVehicle, const std::vector<hta::CVector>& points)
    {
        hta::m3d::AIParam p1 {};
        p1.id = 56;
        p1.Type = hta::m3d::eAIParamType::AIPARAM_ID;

        // Notify AI system that cinematic is starting
        hta::ai::ProcessManager::Instance()->PostMessageA(
            hta::ai::eGameEvent::GE_SUBSCRIBE,      // eventId
            hta::ai::Player::Instance()->m_objId,   // recipientObjId
            self->m_objId,                          // senderObjId
            0.0f,                                   // timeOut
            p1,
            hta::m3d::AIParam{},
            1                                       // framesToPass
        );
    
        hta::m3d::AIParam p2 {};
        p2.id = 57;
        p2.Type = hta::m3d::eAIParamType::AIPARAM_ID;

        hta::ai::ProcessManager::Instance()->PostMessageA(
            hta::ai::eGameEvent::GE_SUBSCRIBE,
            hta::ai::Player::Instance()->m_objId,
            self->m_objId,
            0.0f,
            p2,
            hta::m3d::AIParam{},
            1
        );

        // Setup cinematic
        auto cinematic = hta::m3d::Application::Instance()->m_cinematic;
        cinematic->LoadDefaults();
        cinematic->SetFlags(31);     // Old: 3 (0b11) -> 31 (0b11111)

        // Change application mode (enter cinematic mode)
        hta::m3d::AuxImpulseInfo impulseInfo(2, 1, -1, 0, 0);
        hta::CMiracle3d::Instance()->OnChangeMode(impulseInfo);  // the v6 param is garbage/unused

        vc3::vector<hta::m3d::CameraPathState> cameraStates{};
        
        size_t numPoints = points.empty() ? 0 : points.size();
        cameraStates.resize(numPoints);
        
        hta::Quaternion IdentityQuaternion{0.0, 0.0, 0.0, 1.0};
        auto ZeroVector = hta::CVector{};
        
        // Build camera path states from input points
        for (size_t i = 0; i < cameraStates.size(); ++i)
        {
            cameraStates[i].m_point = points[i];
            cameraStates[i].m_rotation = IdentityQuaternion;
            cameraStates[i].m_zoom = 1.0f;
        }

        // Ensure at least one camera state exists (current camera position)
        if (cameraStates.empty())
        {
            hta::m3d::CameraPathState defaultState;
            defaultState.m_point = ZeroVector;
            defaultState.m_rotation = IdentityQuaternion;
            defaultState.m_zoom = 1.0f;
            cameraStates.resize(1, defaultState);
        }

        // Set first state to current camera position
        cameraStates[0].m_point = hta::m3d::Application::Instance()->m_curCamera.m_worldOrigin;
        cameraStates[0].m_rotation = IdentityQuaternion;
        cameraStates[0].m_zoom = 1.0f;

        // Configure and start cinematic
        cinematic->SetCameraStates(cameraStates);
        cinematic->SetLookTo(1);
        cinematic->SetAimToID(pVehicle->m_objId);
        cinematic->SetWaitWhenStop(1);
        cinematic->Play(7.0f);
        cinematic->StartCinematic();
    }

    // ai::Submarine class method
    void __fastcall _SetSubmarineState(hta::ai::Submarine* self, void* _, hta::ai::eSubmarineState newState)
    {
        auto vehicle = hta::ai::Player::Instance()->GetVehicle();
        if (!vehicle)
            return;

        const auto protoInfo = self->GetPrototypeInfo();

        if (newState == hta::ai::eSubmarineState::SUBMARINE_MOVES)
        {
            self->SetNodeAction(0, 1);

            // Fill camera path from port position
            vc3::vector<hta::m3d::CameraPathState> cameraStates{};
            self->_FillCameraStates(cameraStates, self->m_portPosition);

            // Setup first cinematic (camera follows submarine)
            auto cinematic = hta::m3d::Application::Instance()->m_cinematic;
            cinematic->LoadDefaults();
            cinematic->SetFlags(19); // Old: 1 [0b01] (only fade out, no fade in?) -> 3 [0b10011]

            hta::m3d::AuxImpulseInfo impulseInfo(2, 1, -1, 0, 0);
            hta::CMiracle3d::Instance()->OnChangeMode(impulseInfo);

            cinematic->SetLookTo(1);
            cinematic->SetAimToID(self->m_objId);
            cinematic->SetWaitWhenStop(1);
            cinematic->SetCameraStates(cameraStates);
            cinematic->Play(10.0f);

            // Resize to single state and set to vehicle position + 25 units up
            cameraStates.resize(1);
            hta::CVector vehiclePos = vehicle->GetPosition();
            
            hta::Quaternion IdentityQuaternion{0.0f, 0.0f, 0.0f, 1.0f};
            
            cameraStates[0].m_point.x = vehiclePos.x;
            cameraStates[0].m_point.y = vehiclePos.y + 25.0f;
            cameraStates[0].m_point.z = vehiclePos.z;
            cameraStates[0].m_rotation = IdentityQuaternion;
            cameraStates[0].m_zoom = 1.0f;

            // Setup second cinematic (camera follows vehicle)
            cinematic->SetFlags(28); // Old: 2 [0b10] (no fade out, only fade in?) -> 28 [0b11100]
            cinematic->SetLookTo(1);
            cinematic->SetAimToID(vehicle->m_objId);
            cinematic->SetWaitWhenStop(1);
            cinematic->SetCameraStates(cameraStates);
            cinematic->Play(10.0f);
            cinematic->StartCinematic();

            vehicle->SetHandBrake();

            // Notify AI system
            hta::m3d::AIParam p1{};
            p1.id = 56;  // 0x38
            p1.Type = hta::m3d::eAIParamType::AIPARAM_ID;

            hta::ai::ProcessManager::Instance()->PostMessageA(
                hta::ai::eGameEvent::GE_SUBSCRIBE,
                hta::ai::Player::Instance()->m_objId,
                self->m_objId,
                0.0f,
                p1,
                hta::m3d::AIParam{},
                1
            );

            hta::m3d::AIParam p2{};
            p2.id = 57;  // 0x39
            p2.Type = hta::m3d::eAIParamType::AIPARAM_ID;

            hta::ai::ProcessManager::Instance()->PostMessageA(
                hta::ai::eGameEvent::GE_SUBSCRIBE,
                hta::ai::Player::Instance()->m_objId,
                self->m_objId,
                0.0f,
                p2,
                hta::m3d::AIParam{},
                1
            );
        }
        else if (newState == hta::ai::eSubmarineState::SUBMARINE_OPENS)
        {
            self->SetNodeAction(1, 1);
        }
        else if (newState == hta::ai::eSubmarineState::SUBMARINE_WAITS)
        {
            hta::m3d::Application::Instance()->m_cinematic->StartCinematic();

            vc3::vector<hta::CVector2> points{};
            const vc3::vector<hta::CVector2>& vehiclePoints = 
                self->m_entryPath.GetVehiclePoints();

            if (!vehiclePoints.empty())
            {
                // Use predefined path, add port position at end
                points = vehiclePoints;
                hta::CVector2 portPos2D{ self->m_portPosition.x, self->m_portPosition.z };
                points.push_back(portPos2D);
            }
            else
            {
                // Create simple 3-point path: vehicle -> intermediate -> port
                hta::CVector vehiclePos = vehicle->GetPosition();

                hta::CVector2 startPos{ vehiclePos.x, vehiclePos.z };
                hta::CVector2 intermediatePos{
                    self->m_portPosition.x + self->m_moveDirection.x * 30.0f,
                    self->m_portPosition.z + self->m_moveDirection.z * 30.0f
                };
                hta::CVector2 endPos{ self->m_portPosition.x, self->m_portPosition.z };

                points.push_back(startPos);
                points.push_back(intermediatePos);
                points.push_back(endPos);
            }

            vehicle->SetExternalPath(points);
            vehicle->LimitMaxSpeed(protoInfo->m_vehicleMaxSpeed);

            // Notify vehicle to follow path
            hta::m3d::AIParam p{};
            p.id = 51;  // 0x33
            p.Type = hta::m3d::eAIParamType::AIPARAM_ID;

            hta::ai::ProcessManager::Instance()->PostMessageA(
                hta::ai::eGameEvent::GE_SUBSCRIBE,
                vehicle->m_objId,
                self->m_objId,
                0.0f,
                p,
                hta::m3d::AIParam{},
                1
            );
        }
        self->m_state = newState;
    }

    void Apply() {
        const kraken::Config& config = kraken::Config::Instance();
        enable_fix = config.cinematic_extend.value;

        if (enable_fix)
        {
            LOG_INFO("Feature enabled");
            // ai::Town::_StartCinematic
            // 0x6F2996 // m3d::Cinematic::SetFlags(m_cinematic, 3); 3->15[0xF] (0b11->0b1111) [4 bytes available]
            //routines::Override(1, (void*)0x006F2996, "\x0F");
            
            routines::Redirect(0x012F, (void*)0x00408290, (void*)&n_SetCinematicFadeParams);
            routines::Redirect(0x00C6, (void *)0x004083C0, (void *)&n_SetCinematicCinemaPanel);
            routines::Redirect(0x008C, (void *)0x00625D30, (void *)&GetFadePeriodForState);
            routines::Redirect(0x0578, (void *)0x0041EA20, (void *)&CinematicClear);
            routines::Redirect(0x0047, (void *)0x00408740, (void *)&n_UpdateCinematic);
            routines::Redirect(0x0397, (void *)0x006F2920, (void *)&_StartCinematic);
            routines::Redirect(0x04FB, (void *)0x00803960, (void *)&_SetSubmarineState);
            
            routines::RemapPtr((void*)0x009C6724, (void*)&HandleCinematic_Hook);
        }
    }
}

// Naked wrapper to handle virtual call - pass `this` and float
void __declspec(naked) HandleCinematic_Hook() {
    __asm {
        mov eax, [esp + 4] // Get float
        push eax // Push for Impl
        
        call kraken::fix::cinematic::HandleCinematic_Impl
                 // Impl's `retn 4` cleans up pushed float
                 // ESP is now back at entry point
        
        ret 4    // Clean caller's original float
    }
}
