# Перенос `Appendix` (Meridian 113 → Ex Machina/HTA)

> Заметки по реверс-анализу. Источник истины — `meridian.pdb` (alias `mer`),
> цель — инжектор Kraken в `hta.exe`/`game.pdb` (alias `hta`).
> Анализ выполнен через lora (`scripts3/_probe.py`).

## TL;DR

Скелет класса перенесён корректно (регистрация класса, PrototypeInfo, LoadFromXML,
vtable-методы, фабрика). **Поведенческое ядро — заглушка или расходится с оригиналом.**
Главный пробел — `ReconstructCallback`: в оригинале это полноценный алгоритм спавна
насадок (~1200 байт), в порте — тривиальный reparent. Плюс `fix::thorncollide`
не подключён в `entry.cpp`.

---

## 1. Раскладка оригинала (meridian.pdb)

### `ai::AppendixPrototypeInfo` (424 байта) — ✅ перенесён верно
| поле | offset | тип |
|------|--------|-----|
| (база `ai::GunPrototypeInfo`) | 0 | 404 байта |
| `m_appendixType` | 404 (0x194) | enum AppendixType |
| `m_lpName` | 408 (0x198) | CStr (12 б) |
| `m_thornForce` | 420 (0x1a4) | float |

Порядок и типы в `include/ext/ai/Appendix.hpp` совпадают.

### `ai::Appendix` (896 байт) — ⚠️ инстанс расходится
Оригинал добавляет к `ai::Gun` (база = 864 байта) **только**:
| поле | offset | тип |
|------|--------|-----|
| `m_appendices` | 864 (0x360) | std::vector<m3d::SgNode*> |
| `m_lpNums` | 880 (0x370) | std::vector<int> |

**Конфиг (`appendixType`/`lpName`/`thornForce`) в инстансе НЕ хранится** —
оригинал читает его из `GetPrototypeInfo()` на лету.

Порт же завёл в инстансе `m_lpName`, `m_thornForce`, `m_appendixType`,
`m_cloneCountOverride`. Это сдвигает `m_appendices`/`m_lpNums` и раздувает класс
(~920 vs 896). Функционально не фатально (класс регистрируется заново и движок не
лезет в эти оффсеты напрямую), но это отход от оригинала, а `m_cloneCountOverride`
и связанная с ним логика — **выдумка порта, в оригинале её нет**.

---

## 2. Сверка методов (mer vs порт)

| метод | оригинал | порт | статус |
|-------|----------|------|--------|
| ctor / dtor | только `Gun::Gun` + векторы | + инициализация выдуманных полей | ⚠️ расхождение по полям |
| `Clone` | 26 б, тривиальный (нет полей для копирования) | копирует выдуманные поля | ⚠️ следствие п.1 |
| `CreateObject`/`CreateTargetObject` | alloc + placement new Gun | аналогично | ✅ |
| `GetClass`/`GetBaseClass`/`GetPrototypeInfo` | тонкие | аналогично | ✅ |
| `isLookAtPoint`/`_LaunchShells` | 5-байтные thunk → Gun | `return Gun::...` | ✅ эквивалент |
| `AppendixPrototypeInfo::LoadFromXML` | SafeStrAttrib(lpName)+atof(thorn)+atoi(type) | те же 3 атрибута | ✅ |
| **`ReconstructCallback`** | **~1200 б, полный алгоритм** | **заглушка (reparent + thornForce)** | ❌ **главный пробел** |
| `SetDependantCfg` | 504 б, читает `Configuration::GetGroupVariants` (выбор варианта) | reparent + матрица кости | ❌ нет логики вариантов |
| `ClearAppendices` | `node->GetGraph()->RemoveNode()` + release узлов (vcall+0x38) | `parent->RemoveChild()` | ⚠️ возможна утечка узлов |
| `DeattachCallback` | `ClearAppendices` + Owner/IsKindOf | + ведение side-map thornForce | ⚠️ механизм thornForce — адаптация |
| `_InternalCreateVisualPart`/`BuildVisualPart`/`LoadRuntimeValues`/`SaveRuntimeValues` | **в оригинале НЕ переопределены** | добавлены в порте | ❓ новая логика, не из Meridian |

### Реальный алгоритм `ReconstructCallback` (appendix.cpp:70-159 оригинала)
1. `owner = GetOwner()`; если null или `!owner->IsKindOf(<class@0xC4E158>)` → выход.
2. `proto = this->GetPrototypeInfo()`; `gs = proto->[0x74]` (глобальный масштаб);
   `ds = <DataServer@0xC79B78>->[0x8AF10]`.
3. **Цикл по всем `VehiclePart` в карте частей машины** (`owner->[0x134]`, std::map<CStr,VehiclePart*>):
   - пропустить себя и части без модели (`part->[0x12C]`);
   - получить модель через DataServer (`ds->vcall[0x18](0x400A, part->model->[0x17C], &mdl)`); если нет — пропустить.
   - **Внутренний цикл по индексам N = 1,2,3…**: имя load point = `m_lpName + N` (CStr).
     - `lpId = mdl->GetLoadPointIdByName(name)`; если `-1` → закончить часть.
     - если `part->IsSuppressedLp(lpId)` → следующий N.
     - `lpScale` = из карты `part->proto->[0xB4]` (std::map<CStr,float>), иначе 1.0.
     - если узлов в `m_appendices` меньше, чем нужно → `CreateNode` (this+0xCC) и
       `m_appendices.push_back`, `m_lpNums.push_back(lpId)`; иначе переиспользовать
       существующий (открепив от старого родителя) и обновить `m_lpNums[i]`.
     - `mat = mdl->GetBoneMatrix(lpId)`; `node->SetOriginAbs(mat.origin)`;
       `node->SetRotation(Quaternion::FromMatrix(mat))`;
       `node->SetScale(lpScale * gs)`.
     - `part->model->AddChild(node)`; `node->vcall[0x50](0,1)` (показать);
       `SetDependantCfg(part->model, mdl, node, lpId)`.

То есть насадки **спавнятся на именованных load point'ах `<lpName>1`, `<lpName>2`…
каждой части машины**, позиционируются по костям и масштабируются. Порт этого не делает.

---

## 3. thornForce / коллизии (`source/fix/thorncollide.cpp`)

- Порт реимплементирует **HTA**-функции (проверено по адресам):
  - `0x0088F700` → `ai::CalcDamageToVehicles` ✅
  - `0x00890430` → `ai::CollideVehiclePartAndVehiclePart` ✅
  - `0x007C49E0` → `dBodyGetPointVel` ✅
  - `0x008C28F0`/`0x008C26B0` — статические хелперы без символа (названы `CalcHitValue`/`CalcSideCoeff` по анализу; резолвятся в шум — это норма).
- В оригинале Meridian аналоги — `ai::CalcDamageToVehicles` (0x55C240, 1356 б) и
  `ai::CollideVehiclePartAndVehiclePart` (0x55C930, 822 б). **thornForce там, вероятно,
  читается прямо из насадки/части, а не из side-map.** Порт использует
  `g_vehicleThornForce` (map<Vehicle*,float>) — это адаптация под HTA. Нужно сверить
  оригинальную CalcDamageToVehicles, чтобы убедиться, что множитель применяется так же
  (где именно: `damageToV2 = hitForce/side * thorn * side * hitValue`).
- **`fix::thorncollide::Apply()` НЕ вызывается** в `source/entry.cpp`, и нет
  `include/fix/thorncollide.hpp`. Фича мертва, пока не подключена.

---

## 4. Multi-PDB в lora (task 4) — ✅ уже работает

`scripts3/lora/_state.py::SessionManager` хранит сессии в dict по `alias`; **все**
tool-функции принимают `alias` и роутятся через `manager.get(alias)`. Проверено:
одновременно загружены `mer` (meridian.pdb) и `hta` (game.pdb), запросы по обоим
работают. Доработка не требуется. Единственное возможное QoL-улучшение — отдельный
MCP-tool `list_targets` (сейчас есть только `manager.list_aliases()` без обёртки).

---

## 4b. Краш при загрузке уровня — РЕШЕНО (cross-heap free)

Симптом: вылет в `operator delete` при `~AppendixPrototypeInfo` →
`~PhysicBodyPrototypeInfo` → `_Tidy()` вектора `vc3::vector<CollisionInfo>`
(во время `PrototypeManager::Clear()` на выгрузке карты).

Причина: `PhysicBodyPrototypeInfo::LoadFromXML` — `NATIVE` (игровая, 0x6180E0).
Игра наполняет `m_collisionInfos`, выделяя буфер движковым менеджером
(`[0xA0988C]->vtable[0x18]`; глобальный `operator new` игры @0x407330 и
`std::allocator` ходят туда же). На выгрузке наш (kraken) деструктор разрушает
`vc3::vector`, а `vc3::allocator` звал **глобальный `operator delete` kraken'а
(CRT)** — глобального override не было. Буфер из движковой кучи → free в CRT
kraken'а → **cross-heap free → краш**. Appendix — первый прототип, чей
деструктор скомпилирован в kraken.dll, поэтому проявилось только сейчас.

Сначала попробовали глобальный override `operator new/delete` → g_mar
(`source/ext/memory.cpp`). **Отклонено:** он перехватывает и статические
инициализаторы kraken (`G_CONFIG = new Config()`, CRT iostream/locale),
которые выполняются в `DllMain` ДО подъёма движка → `Kernel::Instance()`
== nullptr → краш при загрузке DLL.

Итоговый фикс (точечный, без глобального override):
1. `vc3::allocator` заведён на движковую кучу. В `vc3/memory` объявлены хуки
   `vc3::_EngineAlloc/_EngineFree`, реализованы в `source/ext/memory.cpp`
   через `Kernel::g_mar`. `_Allocate` и `allocator::deallocate` зовут их.
   vc3-контейнеры аллоцируют только в рантайме (пустой ctor не аллоцирует),
   так что при старте DLL хуки не вызываются. Чинит m_collisionInfos и все
   родственные vc3-векторы (`GunPrototypeInfo::m_fireLpMatrices` и т.п.).
2. `AppendixPrototypeInfo` получил собственные `operator new/delete` → g_mar:
   объект прототипа мы выделяем через g_mar, а движок удаляет его scalar
   deleting destructor'ом → `operator delete`; без member-оператора это был бы
   CRT-free g_mar-указателя. Placement-new для прототипа переведён на `::new`
   (member operator new скрывает глобальный placement).
3. `~CStr` (CStr.cpp) уже освобождает через g_mar — строковые члены безопасны.

Собирается без ошибок. Прежний глобальный override из memory.cpp убран.

## 4c. Breakpoint в PoolManager_Free при выгрузке — РЕШЕНО (битый free set'а)

После фикса 4b игра грузится, но в `PrototypeManager::Clear` (line 76 = per-prototype
scalar deleting destructor) срабатывал debug-`int3` движкового пула. В стеке не было
kraken-фреймов → значит наш Appendix-прототип (первый прототип с kraken-vtable, т.е.
первый раз исполняется kraken-`~VehiclePartPrototypeInfo`) **повреждал пул**, и порчу
ловил следующий free игрового прототипа в том же цикле.

Причина: `VehiclePart.cpp::~VehiclePartPrototypeInfo` обрабатывал `m_loadPoints`
(`vc3::set<CStr>`, красно-чёрное дерево) как плоский буфер:
`g_mar.FreeMem(&*m_loadPoints.begin())` — это адрес CStr **внутри узла** (node+offset),
невалидное начало блока → порча пула. Плюс `_Myhead` (sentinel) ненулевой даже у
пустого set'а, так что блок выполнялся всегда.

Фикс: удалён ручной блок; очистку делает деструктор `vc3::set` (`~_Tree`→`_Tidy`),
который рекурсивно освобождает узлы через `vc3::allocator` (== g_mar благодаря 4b).
Подтверждает, что правку vc3→g_mar откатывать НЕЛЬЗЯ — она нужна и здесь.

## 5. План: чего не хватает (по убыванию важности)

1. **`ReconstructCallback`** — перенести полный алгоритм (раздел 2). Это ядро фичи.
   Нужны хелперы движка в `extern/hta`: `PhysicBody::CreateNode`, `GetBoneMatrix`,
   `GetLoadPointIdByName`, `IsSuppressedLp`, доступ к карте частей `owner->[0x134]`,
   `Configuration::GetGroupVariants`.
2. **Подключить `fix::thorncollide`**: создать `include/fix/thorncollide.hpp`,
   добавить `#include` и вызов `fix::thorncollide::Apply()` в `entry.cpp`.
3. **`SetDependantCfg`** — добавить логику `Configuration::GetGroupVariants`
   (выбор визуального варианта насадки), сейчас отсутствует.
4. **`ClearAppendices`** — переключить на `SceneGraph::RemoveNode` + освобождение
   узлов, как в оригинале (иначе потенциальная утечка клонов).
5. **Решить судьбу инстанс-полей**: либо привести к оригиналу (конфиг только в
   PrototypeInfo), либо осознанно оставить кэш в инстансе и задокументировать.
   `m_cloneCountOverride` + `BuildVisualPart`/`_InternalCreateVisualPart`/
   `Load/SaveRuntimeValues` — выдумка; подтвердить, нужны ли они, или удалить.
6. **Сверить `CalcDamageToVehicles`** оригинала vs порт по применению thornForce.

## 6. Перенос боевой/визуальной логики — СДЕЛАНО (нужен тест в игре)

1. **`fix::thorncollide` подключён**: создан `include/fix/thorncollide.hpp`,
   добавлены `#include` и `fix::thorncollide::Apply()` в `entry.cpp`. Теперь
   thornForce-урон (само оружие ближнего боя) активен.
2. **`ReconstructCallback` переписан** под реальный алгоритм (адаптация под HTA):
   - owner → `IsKindOf("ComplexPhysicObj")`; публикуем thornForce для Vehicle;
   - идём по `ComplexPhysicObj::m_vehicleParts` (напрямую, т.к. `begin()/end()`
     класса в extern/hta не реализованы — только объявлены);
   - для каждой части (кроме себя, с моделью) ищем load point'ы `<LoadPoint><N>`
     (`GetLoadPointIdByName`), клонируем `m_barrelNode`, ставим по `GetBoneMatrix`
     (origin+rotation), `AddChild` к `part->m_Node`; переиспользуем/обрезаем узлы.
   - **Отличия от оригинала (HTA беднее Meridian):** нет карты масштабов по LP →
     scale=1.0; нет `IsSuppressedLp` → проверка опущена; `gs` (глобальный масштаб
     прототипа) не применяется; `Configuration::GetGroupVariants` (варианты в
     `SetDependantCfg`) не переносится — в HTA этого механизма нет.
3. **`ClearAppendices`** → освобождение через `SgNode::GetGraph()->RemoveNode`.
4. **`_InternalCreateVisualPart`** больше не вызывает `BuildVisualPart`
   (пред-спавн клонов в origin убран); спавн только из `ReconstructCallback`.

Статус: компилируется. **Требуется проверка в игре** на машине с установленным
Appendix-оружием (нужен XML-прототип класса "Appendix" с атрибутом `LoadPoint`,
и модель-часть с load point'ами `<LoadPoint>1`, `<LoadPoint>2`…). Открытые вопросы:
корректность обхода `vc3::map` по дереву, привязка к `part->m_Node`, мировые vs
локальные координаты `GetBoneMatrix`.

## 7. thorncollide: краш и починка (CalcHitValue + ABI)

При включении `thorncollide` — краш (AV, EIP в мусоре) в `CalcHitValue`, вызов из
нативной `CollideVehicleAndLandscape` (машина↔земля, срабатывает постоянно).

Две причины:
1. **Мёртвые адреса хелперов.** `CalcHitValue`/`CalcSideCoeff` звали `0x8C28F0`/
   `0x8C26B0` как `__cdecl(float)`. Это **Meridian**-адреса; в HTA там DirectShow-код
   (`CBaseFilter::~CBaseFilter` и пр.). В HTA функций `ai::CalcHitValue`/`CalcSideCoeff`
   нет вовсе. Расшифровал оригиналы из Meridian:
   - `CalcHitValue(speed)`: `speed < порог` → 0; иначе `coeff * speed`.
   - `CalcSideCoeff(dot)`: `dot > 0.707 (cos45)` → frontCoeff; иначе glancingCoeff.
   Параметры в Meridian — рантайм-конфиг (статически не читаются), поэтому
   **заинлайнил формулы, параметры вынес в `kraken.ini [thorncollide]`**:
   `hit_speed_threshold`, `hit_damage_coeff`, `side_threshold`,
   `side_front_coeff`, `side_glancing_coeff` (+ поля в `Config`).
2. **Несовпадение ABI.** Реальная `ai::CalcDamageToVehicles` =
   `void __fastcall(v1, v2, contacts, dSpeed, damageInfo, contactPos)` — 6 арг,
   `ret 0x10`. Порт был `double, 5 арг, ret 0xC` → очистка стека короче на 4 байта →
   порча стека нативного вызывающего. Исправил: сигнатура void/6-арг; `damageToV2`
   теперь отдаётся через файловый статик `g_thornDamageToV2` (читает наш
   `CollideVehiclePartAndVehiclePart`); 6-й арг `contactPos` телом игнорируется
   (контакт берётся из `contacts`). Формула урона не менялась.

Статус: собирается, `thorncollide` снова включён в entry.cpp. Дефолты констант —
ориентировочные (точные Meridian-значения рантаймные); тюнить через kraken.ini.

## 8. Перенос игровых данных (Meridian → рабочая копия HTA)

Источник: `F:\Hard Truck Apocalypse Rise of Clans RUS\data` (Meridian 113).
Цель: `E:\Hard Truck Apocalypse meridian comrem\data` (рабочая копия HTA).

Сделано:
1. **Модели** `thorn1/2/3` (.gam + .sam + текстуры, от моддера из
   `Downloads/thorns_old/thorns`) → `data/models/guns/thorns/`.
2. **Регистрация моделей** в `data/models/animmodels.xml`: добавлены `id="thorn1/2/3"`
   → `.gam` (HTA-спецоружие Turboakselerator тоже регистрируется как `.gam`).
3. **Прототипы**: создан `data/gamedata/gameobjects/specialguns.xml` с 3 Appendix
   (thorn1/2/3, `LoadPoint="LP_THORN"`, ThornForce 25/50/75, AppendixType=1).
   Не-Appendix классы из Meridian-specialguns (TurboAccelerationPusher и т.п.) НЕ
   переносились — их нет в kraken.
4. **Регистрация файла**: в `gameobjects.xml` (группа Guns) добавлен
   `<Folder Name="SpecialGuns" File="specialguns.xml" />`.

Загрузка прототипа: `gameobjects.xml` → `specialguns.xml` → `Class="Appendix"` →
`meta.cpp::CreatePrototypeInfoByClassName` → `CreateAppendixPrototypeInfo`. ✓

Краш при старте (assert `duplicate prototype 'thorn1'` в `_ReadNewPrototype`):
в рабочей копии уже был одиночный `thorn1` (Class=Appendix) в `sideguns.xml`
(прежняя незавершённая попытка, Price=1120/Durability=25). Конфликтовал с моим
specialguns. Резолв: удалил stray-`thorn1` из `sideguns.xml`, оставил полный
комплект в `specialguns.xml`. (`Unknown firing type 'Thorns'` в логе — только
warning, не краш.)

Известные расхождения HTA vs Meridian (не блокеры):
- `FiringType="Thorns"` в HTA неизвестен → `Str2FiringType` вернёт 13
  (`FT_NUM_FIRING_TYPES`, сентинел). Не крашит; для пассивного melee-оружия безвредно.
- `AppendixScale` (атрибут машины в Meridian vehicles.xml) = «gs» — в HTA-прототипе
  машины такого поля нет, не парсится; мой `ReconstructCallback` и так берёт scale=1.0.

Для теста нужно (вне моей зоны):
- свежий `kraken.dll` с фиксами thorncollide/ReconstructCallback задеплоить в раб.копию;
- модель скаута должна иметь кости-лоадпоинты `LP_THORN1`, `LP_THORN2`… (сделал моддер);
- получить шип-оружие на машину (магазин/автоген; CanBeUsedInAutogenerating=1).
- `[thorncollide]` в `data/kraken.ini` допишется сам при первом запуске (дефолты Config).

## Как воспроизвести анализ
```powershell
py -3 e:\KrakenWorkspace\scripts3\_probe.py find                 # типы Appendix в обеих PDB
py -3 e:\KrakenWorkspace\scripts3\_probe.py overview "ai::Appendix" mer
py -3 e:\KrakenWorkspace\scripts3\_probe.py disasm "ai::Appendix::ReconstructCallback" mer
py -3 e:\KrakenWorkspace\scripts3\_probe.py multi <name1> <name2> ... <alias>
```
