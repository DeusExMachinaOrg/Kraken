#pragma once
#include "Obj.h"
#include "Formation.h"
#include "Vehicle.h"
#include "AIPassageState.h"
#include "AIMessage.h"

namespace ai {
    class DecisionMatrix;

    struct AI {
        stable_size_vector<ai::AIPassageState> m_StateStack2;
        stable_size_vector<ai::AIPassageState> m_StateStack1;
        ai::DecisionMatrix* m_pDM;
        bool m_fStateStack2Changed;
        // padding byte
        // padding byte
        // padding byte
        stable_size_vector<ai::AIMessage> m_Messages2;
        stable_size_vector<ai::AIMessage> m_Messages1;
        stable_size_vector<ai::AIMessage> m_Commands;
        int m_numCurCommand;
        bool m_CommandProcessed;
        bool m_CommandStackOpen;
        // padding byte
        // padding byte
    };
    ASSERT_SIZE(ai::AI, 0x60);
}