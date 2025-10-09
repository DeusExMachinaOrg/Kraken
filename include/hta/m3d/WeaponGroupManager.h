#pragma once

#include "Object.h"
#include "utils.hpp"

struct WeaponGroup : m3d::Object
{
	int GroupId;
	int ImpulseId;
	stable_size_set<CStr> GunPartNames;
};

struct WeaponGroupManager : m3d::Object
{
	stable_size_map<WeaponGroup*, int> WeaponGroups;
	stable_size_map<WeaponGroup*, int> SavedWeaponGroups;
};