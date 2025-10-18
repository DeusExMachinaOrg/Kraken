#pragma once
#include "utils.hpp"
#include "hta/m3d/Object.h"
#include "hta/m3d/AIParam.hpp"
#include "hta/ai/Obj.h"

namespace m3d::cmn {
    struct XmlFile;
    struct XmlNode;
};

namespace ai {
    struct DecisionMatrix;

    struct AIParamRef {
        /* Size=0x4 */
        /* 0x0000 */ int32_t m_Num;

        AIParamRef(int32_t);
        AIParamRef();
    };

    class AIPassageCommand {
        /* Size=0x14 */
        /* 0x0000 */ public: int32_t m_StateNum;
        /* 0x0004 */ public: stable_size_vector<AIParamRef> m_ParamRefList;
        
        void Dump();
        AIPassageCommand(const AIPassageCommand&);
        AIPassageCommand();
        ~AIPassageCommand();
    };

    class DecisionMatrixElement {
        /* Size=0x14 */
        /* 0x0000 */ public: stable_size_vector<AIPassageCommand> m_PassageCommands;
        /* 0x0010 */ public: uint16_t m_flags;
    
        DecisionMatrixElement(const DecisionMatrixElement&);
        DecisionMatrixElement();
        void Dump();
        ~DecisionMatrixElement();
    };

    class AIState {
        /* Size=0x64 */
        /* 0x0000 */ CStr m_name;
        /* 0x000c */ stable_size_vector<AIParamRef> m_ParamRefList;
        /* 0x001c */ DecisionMatrix* m_pChildDecisionMatrix;
        /* 0x0020 */ int32_t m_FuncNum;
        /* 0x0024 */ uint32_t m_SignalIDs[16];
    
        AIState(const AIState&);
        AIState();
        void Set(const CStr&, int32_t);
        void SetRetValueInterpretation(int32_t, uint32_t);
        const CStr& GetName() const;
        void Dump() const;
        ~AIState();
    };

    class AISignal {
        /* Size=0x20 */
        /* 0x0000 */ int32_t m_FuncNum;
        /* 0x0004 */ stable_size_vector<AIParamRef> m_ParamRefList;
        /* 0x0014 */ CStr m_name;
        
        AISignal(const AISignal&);
        AISignal();
        void Set(const CStr&, int32_t);
        void Set(const CStr&);
        void Dump() const;
        const CStr& GetName() const;
        ~AISignal();
    };

    class DecisionMatrix : public m3d::Object { /* Size=0xd4 */
        /* 0x0000: fields for m3d::Object */
        /* 0x0034 */ stable_size_vector<AIParamRef> m_tmpParamRefList;
        /* 0x0044 */ stable_size_vector<AIState> m_States;
        /* 0x0054 */ stable_size_vector<AISignal> m_Signals;
        /* 0x0064 */ stable_size_vector<DecisionMatrixElement> m_Elements;
        /* 0x0074 */ int32_t m_numStates;
        /* 0x0078 */ int32_t m_numSignals;
        /* 0x007c */ int32_t m_ExternSignalMappings[16];
        /* 0x00bc */ AIPassageCommand m_Default;
        /* 0x00d0 */ int32_t m_ExitStateNum;
        
        static m3d::Class m_classDecisionMatrix;
        
        DecisionMatrix();
        DecisionMatrix(const DecisionMatrix&);
        virtual ~DecisionMatrix();
        virtual m3d::Object* Clone();
        virtual m3d::Class* GetClass() const;
        void Create(int32_t, int32_t);
        void AddState(const char*, const char*);
        void AddState(const AIState&);
        void AddSignal(const char*, const char*, const char*);
        void AddSignal(const AISignal&);
        void AddCommand(const char*, const char*, const char*);
        void AddCommand(uint32_t, uint32_t, uint32_t, const stable_size_vector<AIParamRef>&);
        void AddSublevel(const char*, m3d::Object*);
        void AddSublevel(uint32_t, DecisionMatrix*);
        void SetSaveStackFlag(const char*, const char*);
        void SetSaveStackFlag(int32_t, int32_t);
        const DecisionMatrixElement* UnsafeGetDecision(int32_t, int32_t) const;
        uint32_t UnsafeFirstDecision(uint32_t, uint32_t) const;
        int32_t GetStateNum(const CStr&) const;
        uint32_t GetSignalNum(const CStr&) const;
        void Dump() const;
        void LogDump() const;
        const DecisionMatrix* GetSubmatrix(int32_t) const;
        const AIState& GetState(int32_t) const;
        const AISignal& GetSignal(int32_t) const;
        int32_t NumStates() const;
        int32_t NumSignals() const;
        int32_t GetExternSignalMapping(int32_t) const;
        const AIPassageCommand& GetDefault() const;
        int32_t GetExitStateNum() const;
        void SetRetValueInterpretation(const char*, const char*, const char*);
        void SetDefaultState(const char*);
        void AddDefaultStateParam(const char*);
        void SetExitState(const char*);
        void FitMatrix();
        void ClearTemporaryParams();
        void AddTemporaryParam(const char*);
        DecisionMatrixElement& _GetElement(int32_t, int32_t);
        const DecisionMatrixElement& _GetElement(int32_t, int32_t) const;

        static m3d::Object* CreateObject();
        static m3d::Class* GetBaseClass();
        static void _LogUnexpectedToken(const CStr&);
    };

    class AIPassageState {
        /* Size=0x14 */
        /* 0x0000 */ int32_t m_StateNum;
        /* 0x0004 */ stable_size_vector<m3d::AIParam> m_ParamList;
        
        void LoadFromXML(m3d::cmn::XmlFile*, const m3d::cmn::XmlNode*);
        void SaveToXML(m3d::cmn::XmlFile*, m3d::cmn::XmlNode*) const;
        AIPassageState(const AIPassageState&);
        AIPassageState();
        ~AIPassageState();
    };

    class AIMessage { /* Size=0x18 */
        /* 0x0000 */ public: int32_t m_Num;
        /* 0x0004 */ public: int32_t m_RemoveAfterFinishing;
        /* 0x0008 */ public: stable_size_vector<m3d::AIParam> m_ParamList;
        
        void LoadFromXML(m3d::cmn::XmlFile*, const m3d::cmn::XmlNode*);
        void SaveToXML(m3d::cmn::XmlFile*, m3d::cmn::XmlNode*) const;
        AIMessage(const AIMessage&);
        AIMessage(int32_t, const m3d::AIParam&, const m3d::AIParam&, const m3d::AIParam&);
        AIMessage();
        bool operator==(const AIMessage&);
        bool operator!=(const AIMessage&);
        ~AIMessage();
    };

    struct AI {
        /* Size=0x60 */
        /* 0x0000 */ stable_size_vector<AIPassageState> m_StateStack2;
        /* 0x0010 */ stable_size_vector<AIPassageState> m_StateStack1;
        /* 0x0020 */ DecisionMatrix* m_pDM;
        /* 0x0024 */ bool m_fStateStack2Changed;
        /* 0x0028 */ stable_size_vector<AIMessage> m_Messages2;
        /* 0x0038 */ stable_size_vector<AIMessage> m_Messages1;
        /* 0x0048 */ stable_size_vector<AIMessage> m_Commands;
        /* 0x0058 */ int32_t m_numCurCommand;
        /* 0x005c */ bool m_CommandProcessed;
        /* 0x005d */ bool m_CommandStackOpen;
    
        AI(const AI&);
        AI();
        void AIInit();
        const CStr& GetCurState2Name();
        const CStr& GetCurState1Name();
        int32_t GetCurState2Num();
        int32_t GetCurState1Num();
        m3d::AIParam GetCmdParam(uint32_t);
        m3d::AIParam GetState1Param(uint32_t);
        m3d::AIParam GetState2Param(uint32_t);
        m3d::AIParam GetMessage1Param(uint32_t);
        m3d::AIParam GetMessage2Param(uint32_t);
        void SetDecisionMatrix(int32_t);
        DecisionMatrix* GetDecisionMatrixPtr();
        void AIUpdate(Obj*);
        void SetState2Param(int32_t, const m3d::AIParam&);
        void SetState1Param(int32_t, const m3d::AIParam&);
        void PutCommand(int32_t, const m3d::AIParam&, const m3d::AIParam&, const m3d::AIParam&);
        void PutCommand(const AIMessage&);
        void InsCommand(int32_t, const m3d::AIParam&, const m3d::AIParam&, const m3d::AIParam&);
        void SetCommand(int32_t, const m3d::AIParam&, const m3d::AIParam&, const m3d::AIParam&);
        void CommandStackOpen();
        void CommandStackClose();
        void PutMessage2(const AIMessage&);
        void PutMessage1(const AIMessage&);
        void LoadAIFromXML(m3d::cmn::XmlFile*, m3d::cmn::XmlNode*);
        void SaveAIToXML(m3d::cmn::XmlFile*, m3d::cmn::XmlNode*) const;
        void Dump();
        CStr ToStr();
        int32_t _CurrentState2Num();
        int32_t _CurrentState1Num();
        int32_t _CurrentMessage2CommandNum();
        int32_t _CurrentMessage1CommandNum();
        ~AI();
    };
};