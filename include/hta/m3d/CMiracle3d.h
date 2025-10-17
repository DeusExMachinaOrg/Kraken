#pragma once
#include "Application.h"
#include "TruxxUiManager.h"
#include "hta/CVector.h"
#include "hta/Quaternion.h"

enum CameraModes : __int32
{
    CM_FIRST_NOTUSED = 0x0,
    CM_BUMPER        = 0x1,
    CM_FOLLOWMODE    = 0x2,
    CM_FLYCAMERA     = 0x3,
    CM_CONST         = 0x4,
    CM_LAST          = 0x5,
};

enum HackedMusicType : __int32
{
    HACKMUSIC_MENU   = 0x0,
    HACKMUSIC_GAME   = 0x1,
    HACKMUSIC_BAR    = 0x2,
    HACKMUSIC_CUSTOM = 0x3,
    HACKMUSIC_LAST   = 0x4,
};

namespace m3d
{
	struct BlockMusicManager;
	struct TownMusicManager;
	struct RadioEngine;
	namespace rend {
		struct TexHandle {
			int m_handle;
		};
	}
}

struct PostEffectManager;
struct ProfileManager;

struct CMiracle3d : m3d::Application
{
	struct CMiracle3d::Player // sizeof=0x28
	{                                       // XREF: CMiracle3d/r
		CameraModes m_cameraMode;
		CVector m_gameLookAt;
		Quaternion m_lastobjQuat;
		int m_elapsedtime;
		float m_desiredDistance;
	};

	enum GameState : __int32
	{
	    GS_GAME           = 0x0,
	    GS_CINEMATIC      = 0x1,
	    GS_MAINMENU       = 0x2,
	    GS_INITIALIZATION = 0x3,
	    GS_NUM_GAMESTATES = 0x4,
	    GS_ERROR          = -0x1,
	};

	struct CMiracle3d::CurGameMode // sizeof=0x4
	{                                       // XREF: CMiracle3d/r
	    GameState m_mode;
	};

	inline static CMiracle3d*& Instance = *(CMiracle3d**)0x00A0A55C;

	CMiracle3d::Player m_player;
	CVector m_oldPositionValue;
	int m_elapsedtime;
	int m_numModals;
	bool m_noclip;
	// padding byte
	// padding byte
	// padding byte
	BYTE _offset1[0x3];
	CVector m_hitPoint;
	CVector m_flyCamTurn;
	CVector m_flyCamMove;
	float m_gameCameraRho;
	CVector m_gameSlideAuto;
	bool m_srvKeys[10];
	bool m_paused;
	// padding byte
	BYTE _offset2[0x1];
	float m_saveTimeScale;
	bool m_userPaused;
	// padding byte
	// padding byte
	// padding byte
	BYTE _offset3[0x3];
	PointBase<float> m_dragHit;
	std::vector<CStr> m_musicNames;
	HackedMusicType m_hackedMusicType;
	bool m_bMustStartNewMusic;
	// padding byte
	// padding byte
	// padding byte
	BYTE _offset4[0x3];
	m3d::BlockMusicManager *m_blockMusicManager;
	m3d::TownMusicManager *m_townMusicManager;
	m3d::RadioEngine *m_radioEngine;
	// padding byte
	// padding byte
	// padding byte
	// padding byte
	BYTE _offset5[0x4];
	void* m_onFinishVideoPlaying;
	// padding byte
	// padding byte
	// padding byte
	// padding byte
	BYTE _offset6[0x4];
	m3d::CVar m_minDist;
	m3d::CVar m_maxDist;
	m3d::CVar m_cameraHeight;
	m3d::CVar m_collideCameraRadius;
	m3d::CVar m_smoothCameraRadius;
	m3d::CVar m_cameraSpeed;
	m3d::CVar m_maxAngle;
	m3d::CVar m_minAngle;
	m3d::CVar m_fov;
	float m_maxTimeScale;
	float m_minTimeScale;
	float m_normalTimeScale;
	bool m_gameInited;
	// padding byte
	// padding byte
	// padding byte
	BYTE _offset7[0x3];
	TruxxUiManager* m_pInterfaceManager;
	m3d::CVar m_cvSoundDebug;
	bool zoomInited;
	// padding byte
	// padding byte
	// padding byte
	BYTE _offset8[0x3];
	float m_Fov0;
	PostEffectManager *m_postEffect;
	bool m_playingVideo;
	// padding byte
	// padding byte
	// padding byte
	BYTE _offset9[0x3];
	ProfileManager *m_profileManager;
	CMiracle3d::CurGameMode m_curGameMode;
	bool m_bBackgroundTextureIsValid;
	bool m_bRenderAsBackground;
	// padding byte
	// padding byte
	BYTE _offset10[0x2];
	m3d::rend::TexHandle m_backgroundTexture;
	// padding byte
	// padding byte
	// padding byte
	// padding byte
	BYTE _offset11[0x4];

	void PutSplash(int proc, const char* text)
	{
		FUNC(0x004039D0, void, __thiscall, _PutSplash, CMiracle3d*, int, const char*);
		_PutSplash(this, proc, text);
	}
};