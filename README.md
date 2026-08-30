<div align="center">

# Kraken

***Расширенный движок*** *для игры [Ex Machina](https://store.steampowered.com/app/285500/Hard_Truck_Apocalypse__Ex_Machina/)*


***Extension engine*** *for the game [Hard Truck Apocalypse](https://store.steampowered.com/app/285500/Hard_Truck_Apocalypse__Ex_Machina/)*

<a id="top"></a>

***

<table>
  <thead>
    <tr>
      <th style="text-align: center;">Содержание</th>
      <th style="text-align: center;">Table of contents (machine translation)</th>
    </tr>
  </thead>
  <tbody align="center">
    <tr>
      <td><a href="#features_ru">Нововведения</a></td>
      <td><a href="#features_en">New features</a></td>
    </tr>
    <tr>
      <td><a href="#installation_ru">Установка</a></td>
      <td><a href="#installation_en">Installation</a></td>
    </tr>
    <tr>
      <td><a href="#luaBinds_ru">Привязка Lua-функций на клавиши</a></td>
      <td><a href="#luaBinds_en">Binding Lua-functions to keys</a></td>
    </tr>
    <tr>
      <td><a href="#wareRepair_ru">Починка машины предметами (как в Меридиане 113)</a></td>
      <td><a href="#wareRepair_en">Repairing with resources (as in Meridian 113)</a></td>
    </tr>
  </tbody>
</table>

</div>

***

<a id="features_ru"></a>

# Нововведения

- Возможность починки предметами как в Меридиан 113
- Возможность привязки выполнения Lua-функций к клавишам клавиатуры
- Исправление заторможенного поведения AI ботов
- Исправление ObjContainer
- Исправление крутящегося кардана
- Вынос констант: гравитация, настройки schwarz, цена покраски и заправки, усиление торможения и ускорения и т.п
- Собственное логирование

<a id="installation_ru"></a><a href="#top">Наверх ↑</a>

# Установка

## С помощью `patcher.py`

1. Скачать [patcher.py](patcher.py) для оригинальной игры или [patcher-comrem.py](patcher-comrem.py) для игры с предустановленным [CommunityPatch/Remaster](https://github.com/DeusExMachinaTeam/EM-CommunityPatch);
2. Прописать внутри `patcher` путь до `.exe` игры. Если в пути содержится кириллица, сохранить в utf-8 кодировке;
3. Выполнить Python'ом;
4. Поместить `kraken.dll` в папку с игрой;
5. Запустить игру;
6. В папке `data` появляется `.ini` с настройками Kraken.

<a id="luaBinds_ru"></a><a href="#top">Наверх ↑</a>

# Привязка Lua-функций на клавиши

Внутри `.ini`:
```ini
[lua_binds]
Enabled=1
Script_1=teleport()
Script_2=g_Console:InputLine("/g_postEffectReload")
```
В самой игре Lua (какой угодно BindKey):
```lua
IMPULSES = GET_GLOBAL_OBJECT "IMPULSES"
IMPULSES:BindKey1("GS_GAME","KEY_0","IM_DEBUG_0")
IMPULSES:BindKey2("GS_GAME","KEY_1","KEY_ALT","IM_DEBUG_1")
```
Где `IM_DEBUG_0` = `Script_1`

<a id="wareRepair_ru"></a><a href="#top">Наверх ↑</a>

# Починка машины предметами (как в Меридиане 113)

Внутри `.ini`:
```ini
[REPAIR_1]
Units=25.000
Armor=4.000
Ware=scrap_metal
Sound=SOUND_REPAIR
[REPAIR_2]
Units=25.000
Ware=machinery
[REFUEL_1]
Units=10.000
Ware=fuel
Sound=SOUND_REFUEL
```
Звук в `data\if\frames\uischema2.xml`:
```xml
<SoundInfo
	Name="SOUND_REPAIR"
	File="data\sounds\other\interface\arm.wav"
	Looped="0"/>

<SoundInfo
	Name="SOUND_REFUEL"
	File="data\sounds\other\interface\oil.wav"
	Looped="0"/>
```

</a><a href="#top">Наверх ↑</a>

---------------------

<a id="features_en"></a>

# New features

- The ability to repair with items, as in Meridian 113
- The ability to bind the execution of Lua functions to keyboard keys
- Fixing the sluggish behavior of AI bots
- Fixing ObjContainer
- Fixing the rotating gimbal
- Moving constants out: gravity, schwarz settings, paint and refuel prices, braking and acceleration boost, etc.
- Custom logging

<a id="installation_en"></a><a href="#top">Go up ↑</a>

# Installation

## With the `patcher.py`

1. Download the [patcher.py](patcher.py) for the original game or [patcher-comrem.py](patcher-comrem.py) for the game with the [CommunityPatch/Remaster](https://github.com/DeusExMachinaTeam/EM-CommunityPatch);
2. Specify the path to the game’s `.exe` file inside the `patcher`;
3. Run it through Python;
4. Place `kraken.dll` in the game folder;
5. Run the game;
6. In the `data` folder, a `.ini` file appears with Kraken settings.

<a id="luaBinds_en"></a><a href="#top">Go up ↑</a>

# Binding Lua-functions to keys

Inside `.ini`:
```ini
[lua_binds]
Enabled=1
Script_1=teleport()
Script_2=g_Console:InputLine("/g_postEffectReload")
```
Inside game Lua (any BindKey):
```lua
IMPULSES = GET_GLOBAL_OBJECT "IMPULSES"
IMPULSES:BindKey1("GS_GAME","KEY_0","IM_DEBUG_0")
IMPULSES:BindKey2("GS_GAME","KEY_1","KEY_ALT","IM_DEBUG_1")
```
Where `IM_DEBUG_0` = `Script_1`

<a id="wareRepair_en"></a><a href="#top">Go up ↑</a>

# Repairing with resources (as in Meridian 113)

Inside `.ini`:
```ini
[REPAIR_1]
Units=25.000
Armor=4.000
Ware=scrap_metal
Sound=SOUND_REPAIR
[REPAIR_2]
Units=25.000
Ware=machinery
[REFUEL_1]
Units=10.000
Ware=fuel
Sound=SOUND_REFUEL
```
Sound in `data\if\frames\uischema2.xml`:
```xml
<SoundInfo
	Name="SOUND_REPAIR"
	File="data\sounds\other\interface\arm.wav"
	Looped="0"/>

<SoundInfo
	Name="SOUND_REFUEL"
	File="data\sounds\other\interface\oil.wav"
	Looped="0"/>
```

<a href="#top">Go up ↑</a>
