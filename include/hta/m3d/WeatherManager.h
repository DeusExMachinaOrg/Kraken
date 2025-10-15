#pragma once
#include "hta/Func.h"
#include "stdafx.hpp"
#include "utils.hpp"
#include "Weather.h"

namespace m3d
{
	enum GlobalTimeParams 
	{
		GTP_SUNRISE_TIME = 0,
		GTP_DAY_TIME = 1,
		GTP_SUNSET_TIME = 2,
		GTP_NIGHT_TIME = 3,
		GTP_NUM_PARAMS = 4
	};

	struct WeatherManager
	{
		stable_size_vector<Weather*> m_weatherStorage;
		stable_size_vector<Weather*> m_curWeatherStorage;
		m3d::Weather* m_currentWeather;
		BYTE _offset[0x18];
		GlobalTimeParams m_curDayTime;
		float m_globalTimeParams[4];
		bool m_bEdit;

		void UpdateDayTime()
		{
			FUNC(0x00660060, void, __thiscall, _UpdateDayTime, WeatherManager*);
			_UpdateDayTime(this);
		}
		void ReadFromXmlFile(const char* name)
		{
			FUNC(0x00660300, void, __thiscall, _ReadFromXmlFile, WeatherManager*, const char*);
			_ReadFromXmlFile(this, name);
		}
		void SetActiveWeather(unsigned int Cur)
		{
			FUNC(0x0065ED70, void, __thiscall, _SetActiveWeather, WeatherManager*, unsigned int);
			_SetActiveWeather(this, Cur);
		}
	};
}