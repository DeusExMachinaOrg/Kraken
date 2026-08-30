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
	<tr>
      <td><a href="#uibooks_ru">Форматирование книг в журнале</a></td>
      <td><a href="#uibooks_en">Formatting books in a journal</a></td>
    </tr>
  </tbody>
</table>

</div>

***

<a id="features_ru"></a>

# Нововведения

- Возможность починки предметами как в Меридиан 113
- Возможность привязки выполнения Lua-функций к клавишам клавиатуры
- Возможность форматирования книг в журнале
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

<a id="uibooks_ru"></a><a href="#top">Наверх ↑</a>

# Форматирование книг в журнале

<div align="center">
	
<img src="uibooks_screens\uibooks_preview1.png" alt="uibooks_preview1.png" width="450" />
<img src="uibooks_screens\uibooks_preview2.png" alt="uibooks_preview2.png" width="450" />

</div>

- Разделение книги на страницы: через `#page` в атрибуте value или автоматически через
```xml
<book id="r4_book_Scarytail1" mode="pages" />
```
`mode="scroll"` для разрешения скролла большого объема текста.

- **Жирный**, *курсивный* и ***жирно-курсивный***: через `*Жирный*`, `_курсивный_` и `*_жирно-курсивный_*`.
- Выравнивание текста: `#left`, `#right` и `#center`.
- Вставка изображений: `![Логотип](data\if\ico_hd\Splashes\comrem_logo.png)`

Пример книги:
```xml
<book id="r4_book_Scarytail1" mode="scroll" />

<string
	id="r4_book_Scarytail1"
	value="DEMO: длинный скролл" />

<string
	id="r4_book_Scarytail1_diz"
	value="#right|Жизнь успешного торговца начала угнетать меня, когда таинственный незнакомец пришёл ко мне и предложил продать карту сокровищ. 
	Приди он на неделю раньше, и я приказал бы выгнать его взашей. Но теперь я с радостью заплатил ему баснословную цену за клочок древней бумаги.
	 Незнакомец вежливо поклонился и ушел в ночь. Почему-то меня не оставляло чувство, что мы с ним ещё увидимся.
	 |Я нанял команду отчаянных парней, подготовился к путешествию, и спустя неделю после ночного визита мы двинулись в путь.
	  Нам пришлось отбивать атаки безумных дикарей, и мы потеряли в схватках одну машину вместе с водителем и припасами.
	  |На 17 день нашего путешествия мы приблизились, наконец, к цели нашего путешествия. 
	  Величественные вершины простирались перед нами всюду, куда не кинешь взор.
	   Едва увидев эту небывалую картину, я сразу понял, что не зря терпел все лишения и опасности пути.
	    А ведь это были только врата в сказочную страну.|Перебраться через горы оказалось труднее, чем мы рассчитывали.
		|#center|![Логотип](data\if\ico_hd\Splashes\comrem_logo.png)|
		 #left|Тоннель, указанный на карте, оказался завален. Мы ничего не смогли сделать с огромной грудой камней, так что пришлось искать обходной путь через горы.
		 |Машины не справляются с перегрузками, горючее кончается. Оставили технику, отправились пешком через перевал.
		  Мой друг @AAFF3333*Свен* сорвался в ущелье и буквально разлетелся на части, упав на _дно_, усеянное острыми льдинами. 
		  |Его маска до сих пор лежит там, глядя в небо с немым укором. Вместе с ним погибла и карта сокровищ, а также наша надежда выбраться когда-нибудь из этого ледяного ада.
		  |Сам не понимаю, как нам удалось найти путь среди воющего ветра и ледяных вершин. 
		  Но скоро мы начали спуск и через 3 дня оказались на цветущей земле *_Либриума_*, так называли этот край доброжелательные аборигены, которые выходили нас.
		  |Эти добрые люди и оказались единственным сокровищем, которое мы обнаружили в нелегком путешествии.
		  |#page
		  |Один раз в 40 лет особо холодной зимой небо покрывается глухими черными тучами так, что солнца совсем не видно и с неба вместо дождя начинают падать
		   белые как саван ледяные хлопья. Они покрывают толстым слоем всю землю.
		    Крыши проваливаются под их весом, деревья ломаются, как спички. Человек, которого эта напасть застанет на улице, 
			замерзает насмерть за считанные минуты. Это называется ‘’снег’’.|Говорят, что где-то далеко на севере есть царство вечного холода.
			 Там в небо поднимаются огромные никогда не тающие кучи ‘’снега’’, чёрные деревья с лишенными листьев ветвями сгибаются под его грузом,
			  и нигде ни травинки, только вечно голодные чудовища рыщут в поисках добычи. Но это, скорее всего, просто пустые россказни, 
			  ведь такая картина слишком страшна, чтобы быть правдивой." />
```

</a><a href="#top">Наверх ↑</a>

---------------------

<a id="features_en"></a>

# New features

- The ability to repair with items, as in Meridian 113
- The ability to bind the execution of Lua functions to keyboard keys
- The ability to books formatting in a journal
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

<a id="uibooks_en"></a><a href="#top">Go up ↑</a>

# Formatting books in a journal

<div align="center">
	
<img src="uibooks_screens\uibooks_preview1.png" alt="uibooks_preview1.png" width="450" />
<img src="uibooks_screens\uibooks_preview2.png" alt="uibooks_preview2.png" width="450" />

</div>

- Dividing the book into pages: via `#page` in the `value` attribute or automatically through
```xml
<book id="r4_book_Scarytail1" mode="pages" />
```
`mode="scroll"` to allow scrolling through a large volume of text.

- **Bold**, *italic*, and ***bold-italic***: through`*Bold*`, `_italic_` и `*_bold-italic_*`.
- Text alignment: `#left`, `#right` и `#center`.
- Inserting Images: `![Logotip](data\if\ico_hd\Splashes\comrem_logo.png)`

Book example (RU):
```xml
<book id="r4_book_Scarytail1" mode="scroll" />

<string
	id="r4_book_Scarytail1"
	value="DEMO: длинный скролл" />

<string
	id="r4_book_Scarytail1_diz"
	value="#right|Жизнь успешного торговца начала угнетать меня, когда таинственный незнакомец пришёл ко мне и предложил продать карту сокровищ. 
	Приди он на неделю раньше, и я приказал бы выгнать его взашей. Но теперь я с радостью заплатил ему баснословную цену за клочок древней бумаги.
	 Незнакомец вежливо поклонился и ушел в ночь. Почему-то меня не оставляло чувство, что мы с ним ещё увидимся.
	 |Я нанял команду отчаянных парней, подготовился к путешествию, и спустя неделю после ночного визита мы двинулись в путь.
	  Нам пришлось отбивать атаки безумных дикарей, и мы потеряли в схватках одну машину вместе с водителем и припасами.
	  |На 17 день нашего путешествия мы приблизились, наконец, к цели нашего путешествия. 
	  Величественные вершины простирались перед нами всюду, куда не кинешь взор.
	   Едва увидев эту небывалую картину, я сразу понял, что не зря терпел все лишения и опасности пути.
	    А ведь это были только врата в сказочную страну.|Перебраться через горы оказалось труднее, чем мы рассчитывали.
		|#center|![Логотип](data\if\ico_hd\Splashes\comrem_logo.png)|
		 #left|Тоннель, указанный на карте, оказался завален. Мы ничего не смогли сделать с огромной грудой камней, так что пришлось искать обходной путь через горы.
		 |Машины не справляются с перегрузками, горючее кончается. Оставили технику, отправились пешком через перевал.
		  Мой друг @AAFF3333*Свен* сорвался в ущелье и буквально разлетелся на части, упав на _дно_, усеянное острыми льдинами. 
		  |Его маска до сих пор лежит там, глядя в небо с немым укором. Вместе с ним погибла и карта сокровищ, а также наша надежда выбраться когда-нибудь из этого ледяного ада.
		  |Сам не понимаю, как нам удалось найти путь среди воющего ветра и ледяных вершин. 
		  Но скоро мы начали спуск и через 3 дня оказались на цветущей земле *_Либриума_*, так называли этот край доброжелательные аборигены, которые выходили нас.
		  |Эти добрые люди и оказались единственным сокровищем, которое мы обнаружили в нелегком путешествии.
		  |#page
		  |Один раз в 40 лет особо холодной зимой небо покрывается глухими черными тучами так, что солнца совсем не видно и с неба вместо дождя начинают падать
		   белые как саван ледяные хлопья. Они покрывают толстым слоем всю землю.
		    Крыши проваливаются под их весом, деревья ломаются, как спички. Человек, которого эта напасть застанет на улице, 
			замерзает насмерть за считанные минуты. Это называется ‘’снег’’.|Говорят, что где-то далеко на севере есть царство вечного холода.
			 Там в небо поднимаются огромные никогда не тающие кучи ‘’снега’’, чёрные деревья с лишенными листьев ветвями сгибаются под его грузом,
			  и нигде ни травинки, только вечно голодные чудовища рыщут в поисках добычи. Но это, скорее всего, просто пустые россказни, 
			  ведь такая картина слишком страшна, чтобы быть правдивой." />
```

<a href="#top">Go up ↑</a>
