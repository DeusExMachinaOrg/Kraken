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
	virtual void Dtor() = 0;
	virtual void Clone() = 0;
	virtual void ReadFromXmlNode() = 0;
	virtual void ReadFromXmlNodeAfterAdd() = 0;
	virtual void WriteToXmlNode() = 0;
	virtual void SetProperty(unsigned int propId, void* prop) = 0;
	virtual void GetProperty(unsigned int propId, void* prop) = 0;
	virtual void GetPropertiesList(stable_size_set<unsigned int>*) = 0;
	virtual void AddChild() = 0;
	
	stable_size_map<WeaponGroup*, int> WeaponGroups;
	stable_size_map<WeaponGroup*, int> SavedWeaponGroups;
};