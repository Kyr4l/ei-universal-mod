----------------------------------------------------------------------------------------------------
-- Стандартная реализация функций для замены в игре. Эти варианты будут использоваться в том случае,
-- если пользователь не переопределит какую-либо из них своей реализацией
----------------------------------------------------------------------------------------------------

require("client_server")

----------------------------------------------------------------------------------------------------
-- Расчет показателя урона
----------------------------------------------------------------------------------------------------

-- На стороне клиента (инфоокно)
function GetModifiedDamageOnClient(unit_client_stats_ptr, unit_client_addr, damage) --> float
    local unit_stats = UnitClientStats(unit_client_stats_ptr)
    local unit       = UnitClient(unit_client_addr)

    local strengthen = unit:spellEffect(SpellType.Strengthen) * 0.01 * 0.3
    local weaken     = unit:spellEffect(SpellType.Weaken)     * 0.01 * 0.3
    local modifier   = (1 + strengthen) / (1 + weaken)

    return math.max(1.0, damage * modifier)
end

-- На стороне сервера
function GetModifiedDamageOnServer(unit_server_stats_ptr, unit_server_addr, damage) --> float
    local unit_stats = UnitServerStats(unit_server_stats_ptr)
    local unit       = UnitServer(unit_server_addr)

    local strengthen = unit:spellEffect(SpellType.Strengthen) * 0.01 * 0.3
    local weaken     = unit:spellEffect(SpellType.Weaken)     * 0.01 * 0.3
    local modifier   = (1 + strengthen) / (1 + weaken)
    
    return math.max(1.0, damage * modifier)
end



----------------------------------------------------------------------------------------------------
-- Расчет количества здоровья и маны
----------------------------------------------------------------------------------------------------

-- Вычисление базового показателя здоровья (с учетом перков)
function CalcBaseHealth(unit_server_stats_addr) --> float
    local unit_stats = UnitServerStats(unit_server_stats_addr)
    
    local hp                = 30 * 1.10^(math.log(unit_stats.experience / 50, 1.61))
    local strength_modifier = unit_stats.strength.curr / 25
    local perk_modifier     = 1 + unit_stats:perkModifier(PerkType.Health) * 0.01

    return strength_modifier * perk_modifier * hp
end

-- Вычисление базового показателя запаса сил (с учетом перков)
function CalcBaseMana(unit_server_stats_addr) --> float
    local unit_stats   = UnitServerStats(unit_server_stats_addr)
    
    local mp = 30 * 1.10^(math.log(unit_stats.experience / 50, 1.61))
    local dexterity_modifier = unit_stats.dexterity.curr / 25
    local perk_modifier     = 1 + unit_stats:perkModifier(PerkType.Mana) * 0.01

    return dexterity_modifier * perk_modifier * mp
end

-- Вычисление здоровья с учетом наложенных заклинаний. Как правило, hp будет
-- вычисляться по формуле выше, либо передаваться из базы данных (для мобов)
function GetModifiedHealth(unit_server_stats_addr, unit_server_addr, hp) --> float
    local unit_stats = UnitServerStats(unit_server_stats_addr)
    local unit       = UnitServer(unit_server_addr)
    
    local strengthen = unit:spellEffect(SpellType.Strengthen) * 0.01 * 0.5
    local weaken     = unit:spellEffect(SpellType.Weaken)     * 0.01 * 0.5
    local modifier = (1 + strengthen) / (1 + weaken)
    return math.max(1, hp * modifier)
end



----------------------------------------------------------------------------------------------------
-- Регенерация здоровья и маны
-- hp_only - true, если эта функция должна провести только регенерацию жизней
----------------------------------------------------------------------------------------------------

-- Регенерация жизней -- fixed the perma regen bug
function RegenerateHp(common_server_stats_addr, unit_server_stats_addr, unit_addr) --> bool
    local common_stats = UnitServerCommonStats(common_server_stats_addr)
    local unit_stats   = UnitServerStats(unit_server_stats_addr)
    local unit         = UnitServer(unit_addr)

    local hp = common_stats.hp
    if hp.curr >= hp.max then return false end

    local hp_regen = hp.regen + hp.regen * unit_stats:perkModifier(PerkType.Vitality) * 0.01
    local regen_spell = unit:spellEffect(SpellType.Regeneration)
    if regen_spell > 0 then
        hp_regen = hp_regen * regen_spell
    end
    unit_stats:heal(common_server_stats_addr, hp.max * hp_regen)
    return true
end

-- Регенерация запаса сил
function RegenerateMp(common_server_stats_addr, unit_server_stats_addr, unit_addr) --> bool
    local common_stats = UnitServerCommonStats(common_server_stats_addr)
    local unit_stats   = UnitServerStats(unit_server_stats_addr)
    local unit         = UnitServer(unit_addr)

    local mp = common_stats.mp
    if mp.curr >= mp.max then return false end
    
    local mp_regen = mp.regen + mp.regen * unit_stats:perkModifier(PerkType.Spirit) * 0.01
    common_stats:regenMp(mp_regen)
    return true
end

-- Общая функция регенерации
function RegenerateHpAndMp(common_server_stats_addr, unit_server_stats_addr,
                           unit_addr, hp_only) --> bool
    local common_stats = UnitServerCommonStats(common_server_stats_addr)
    local unit_stats   = UnitServerStats(unit_server_stats_addr)
    local unit         = UnitServer(unit_addr)

    -- По-умолчанию регенерация вызывается один раз в 15 тиков
    common_stats.regenCounter = common_stats.regenCounter + 1
    if (common_stats.regenCounter > 0) and
       (common_stats.regenCounter < 15)
    then
        return false
    end
    common_stats.regenCounter = 0
    
    -- Регенерация очков жизней
    local hp_result = RegenerateHp(common_server_stats_addr, unit_server_stats_addr, unit_addr)
    -- Нужно ли регенить ману?
    if hp_only then return hp_result end
    -- Регенерация маны
    local mp_result = RegenerateMp(common_server_stats_addr, unit_server_stats_addr, unit_addr)
    -- Если отработала хотя бы одна функция, вернется true
    return hp_result or mp_result
end



----------------------------------------------------------------------------------------------------
-- Стоимость навыков и умений
----------------------------------------------------------------------------------------------------

-- Необходимое количество опыта для взятие навыка уровня level с нуля
function GetSkillPrice(level) --> float
    return 46 * 1.61 ^ (level / 5)
end

-- Необходимое количество опыта для взятия навыка уровня to с уровня from
function GetSkillPriceDiff(from, to) --> int
    return math.floor(GetSkillPrice(to) - GetSkillPrice(from))
end

-- Необходимое количество опыта для взятия конкретного умения
function GetPerkPrice(unit_server_stats_addr, perk_addr) --> int
    local unit_stats   = UnitServerStats(unit_server_stats_addr)
    local perk         = PerksDatabase.PerksTableRecord(perk_addr)
    
    local index      = PerksDatabase.PerksTable.getRecordIndex(perk)
    local base       = IDiv(index, 3)
    local fraction   = IMod(index, 3)
    local base_price = 0
    
    -- Получение "базовой" цены умения
    if fraction > 0 then
        local prev_perk = PerksDatabase.PerksTable.getRecord(base * 3 + fraction - 1)
        assert(prev_perk)
        base_price = GetSkillPriceDiff(prev_perk.cost, perk.cost)
    else
        base_price = GetSkillPriceDiff(0, perk.cost)
    end
    
    -- Сумма уровней всех умений
    local perks_count = 0
    for i = 0, 35 do
        perks_count = perks_count + unit_stats.perks:value(i)
    end
    base_price = base_price * (2 ^ perks_count)
    
    -- Округление до двух старших разрядов (1`234`567 -> 12`000`000)
    local dec_base = 10 ^ math.floor(math.log10(base_price))
    if base_price / dec_base < 4 then
        if dec_base <= 10 then
            return math.ceil(base_price)
        else
            dec_base = dec_base * 0.1     
        end
    end
    return math.floor(base_price / dec_base + 0.5) * dec_base
end



----------------------------------------------------------------------------------------------------
-- Расход запаса сил во время бега
----------------------------------------------------------------------------------------------------

function CalcRunningManaCost(unit_server_stats_addr, unit_server_addr, common_server_stats_addr) --> none
    local unit_stats = UnitServerStats(unit_server_stats_addr)
    local unit = UnitServer(unit_server_addr)
    local common_stats = UnitServerCommonStats(common_server_stats_addr)
    local mp = common_stats.mp

    mp.curr = mp.curr - mp.max * (1 / 150)
end



----------------------------------------------------------------------------------------------------
-- Применение эффекта от изученных умений и наложенных заклинаний
----------------------------------------------------------------------------------------------------

-- Расчет модификатора ночного зрения
function GetNightVisionModifier(unit_stats) --> float
    local perk_mod = unit_stats:perkModifier(PerkType.Infra)
    return unit_stats:sense(SenseType.Infra).curr * perk_mod / 100
end

-- Вычисление максимальной нагрузки персонажа
-- Внимание: не меняйте алгоритм этой функции
function GetMaxEncumbrance(unit_stats) --> float
    local result = unit_stats.strength.curr * 12
    local perk_mod = unit_stats:perkModifier(PerkType.Lift) * 0.01
    return result + result * perk_mod
end

-- Уменьшение действий (через коэффициент) в случае раненой или отрубленной конечности
-- Внимание: не меняйте алгоритм этой функции
function RecalcDamageModifier(unit_stats, body_part) --> float
    local min_percentage = 1
    for i = 0, 5 do
        local hl = unit_stats:hitLocation(i)
        if (hl.state ~= ArmorLayerState.NotUsed) and
           (hl.type == body_part)                and
           (hl.hp < hl.maxHp)
        then
            min_percentage = math.min(min_percentage, hl.hp / hl.maxHp)
        end
    end
    if min_percentage <= 0.0 then
        return 0.33
    elseif min_percentage <= 0.5 then
        return 0.5
    else
        return 1
    end
end

-- Сброс значений изменяемых характеристик до базовых
function SetDefaultStats(unit_stats, common_stats) --> none
    unit_stats.strength.curr     = unit_stats.strength.base
    unit_stats.dexterity.curr    = unit_stats.dexterity.base
    unit_stats.intelligence.curr = unit_stats.intelligence.base
    
    for i = 0, 5 do
        unit_stats:sense(i).curr   = unit_stats:sense(i).base
        unit_stats:percept(i).curr = unit_stats:percept(i).base
    end
    for i = 0, 6 do
        common_stats:absorbtion(i).curr = common_stats:absorbtion(i).base
    end
end

-- Применение умений
function ApplyPerks(unit_stats) --> none
    -- Ночное зрение
    unit_stats:sense(SenseType.Infra):addCurr(GetNightVisionModifier(unit_stats))
    -- Сила
    unit_stats.strength:addCurr(unit_stats:perkModifier(PerkType.Strength))
    -- Ловкость
    unit_stats.dexterity:addCurr(unit_stats:perkModifier(PerkType.Dexterity))
    -- Разум
    unit_stats.intelligence:addCurr(unit_stats:perkModifier(PerkType.Intelligence))
end

-- Применение небоевых заклинаний с продолжительностью и эффектом
function ApplySpellEffects(unit_stats, common_stats, spell_effect_mods) --> none
    -- Меняем название на более короткое для удобства
    local sem = spell_effect_mods

    -- Защита от магии
    
    -- Стихийные защиты не должны складываться с защитой от магии, выбираем наибольшее
    local magic_prot     = sem:value(SpellType.Antimagic)
    local fire_prot      = math.max(magic_prot, sem:value(SpellType.ProtFire))
    local lightning_prot = math.max(magic_prot, sem:value(SpellType.ProtElectric))
    local acid_prot      = math.max(magic_prot, sem:value(SpellType.ProtAcid))
    -- Защита от огня
    common_stats:absorbtion(DamageType.Termical):addCurr(fire_prot)
    -- Защита от молнии
    common_stats:absorbtion(DamageType.Electrical):addCurr(lightning_prot)
    -- Защита от кислоты
    common_stats:absorbtion(DamageType.Chemical):addCurr(acid_prot)

    -- Бафы

    -- Орлиный взор
    unit_stats:sense(SenseType.Sight):addCurr(sem:value(SpellType.EagleSight))
    -- Ночное зрение
    unit_stats:sense(SenseType.Infra):addCurr(sem:value(SpellType.Infravision))
    -- Зрение мертвеца
    unit_stats:sense(SenseType.SenseLife):addCurr(sem:value(SpellType.DetectLife))
    -- Зловоние (неиспользуемое заклинание)
    unit_stats:percept(SenseType.Smell):addCurr(sem:value(SpellType.Stench))
    -- Невидимость
    unit_stats:percept(SenseType.Sight).curr = math.max(0,
        unit_stats:percept(SenseType.Sight).curr - sem:value(SpellType.Invisibility) / 100)
    -- Тихий шаг
    unit_stats:percept(SenseType.Hearing).curr = math.max(0,
        unit_stats:percept(SenseType.Hearing).curr - sem:value(SpellType.Silence))
    -- Мертвец
    unit_stats:percept(SenseType.SenseLife).curr = math.max(0,
        unit_stats:percept(SenseType.SenseLife).curr - sem:value(SpellType.Lichdom))
end

-- Вместо этой пустой функции в main.lua можно прописать что-то полезное
function ApplyCustomEffects(unit, unit_stats, common_stats, spell_effect_mods) --> none
end

-- Основная функция
function RecalcUnitStats(unit_server_stats_addr, common_server_stats_addr,
                         unit_addr, spell_effect_mods_addr) --> none
    local unit_stats        = UnitServerStats(unit_server_stats_addr)
    local common_stats      = UnitServerCommonStats(common_server_stats_addr)
    local unit              = UnitServer(unit_addr)
    local spell_effect_mods = Array(spell_effect_mods_addr, Memory.Type.Float, 40, true)

    SetDefaultStats(unit_stats, common_stats)
    ApplyPerks(unit_stats)
    ApplySpellEffects(unit_stats, common_stats, spell_effect_mods)
    ApplyCustomEffects(unit, unit_stats, common_stats, spell_effect_mods)

    -- Перерасчет очков жизней и запаса сил
    if unit.isPlayer then
        common_stats.hp.max = CalcBaseHealth(unit_server_stats_addr)
        common_stats.mp.max = CalcBaseMana(unit_server_stats_addr)
    else
        common_stats.hp.max = unit_stats.baseHp
    end
    common_stats.hp.max = GetModifiedHealth(unit_server_stats_addr, unit_addr, common_stats.hp.max)
    unit_stats:distributeHp(common_stats.hp.max)

    -- Перерасчет нагрузки
    if unit.isPlayer then
        unit_stats.visionArc      = -2 -- Круговое зрение
        unit_stats.maxEncumbrance = GetMaxEncumbrance(unit_stats)
    else
        unit_stats.maxEncumbrance = 1000000
    end
    unit_stats.currEncumbrance = unit.itemsWeight

    -- Перерасчет действий
    unit_stats.actionsDamageModifier = RecalcDamageModifier(unit_stats, BodyPartType.Hand)
    if unit.isPlayer then
        unit_stats.actions.base = 10 + unit_stats.dexterity.curr * 0.2
    else
        unit_stats.actions.base = 15
    end
    -- Разница между ускорением и замедлением
    local speed_slow_diff = spell_effect_mods:value(SpellType.Speed) -
        spell_effect_mods:value(SpellType.Slow)
    -- Модификатор умения
    local actions_perk_mod = unit_stats:perkModifier(PerkType.Quickness) / 100
    -- Действия с учетом умения
    unit_stats.actions.curr = unit_stats.actions.base * (1 + actions_perk_mod) *
        unit_stats.actionsDamageModifier
    -- Действия с учетом заклинаний
    unit_stats.actions.curr = math.max(1, unit_stats.actions.curr + speed_slow_diff)

    -- Перерасчет скорости передвижения
    unit_stats.moveDamageModifier = 1
    if unit_stats.locomotion == LocomotionType.Walk then
        unit_stats.moveDamageModifier = RecalcDamageModifier(unit_stats, BodyPartType.Leg)
    end
    -- unit_stats.move.base берется из базы
    unit_stats.move.curr = unit_stats.move.base * unit_stats.moveDamageModifier

    -- Окончательный перерасчет очков жизней. Эта функция вызывается
    -- после установления damageModifier для рук и ног
    common_stats.hp.curr = unit_stats:collectHp(common_stats.hp.max)
end

-- Вызывается сразу же после создания героя в редакторе
function RecalcInternalHeroStats(unit_server_stats_addr, items_weight) --> none
    local unit_stats = UnitServerStats(unit_server_stats_addr)

    unit_stats.strength.curr     = unit_stats.strength.base
    unit_stats.dexterity.curr    = unit_stats.dexterity.base
    unit_stats.intelligence.curr = unit_stats.intelligence.base
    
    for i = 0, 5 do
        unit_stats:sense(i).curr   = unit_stats:sense(i).base
        unit_stats:percept(i).curr = unit_stats:percept(i).base
    end

    ApplyPerks(unit_stats)
    unit_stats.visionArc = -2 -- Круговое зрение
    unit_stats:distributeHp(CalcBaseHealth(unit_server_stats_addr))

    unit_stats.currEncumbrance       = items_weight
    unit_stats.maxEncumbrance        = GetMaxEncumbrance(unit_stats)
    unit_stats.moveDamageModifier    = RecalcDamageModifier(unit_stats, BodyPartType.Leg)
    unit_stats.move.curr             = unit_stats.move.base * unit_stats.moveDamageModifier
    unit_stats.actionsDamageModifier = RecalcDamageModifier(unit_stats, BodyPartType.Hand)
    unit_stats.actions.base          = 10 + unit_stats.dexterity.curr * 0.2

    local actionsPerkModifier = 1 + unit_stats:perkModifier(PerkType.Quickness) / 100
    unit_stats.actions.curr = math.max(1, unit_stats.actions.base *
        unit_stats.actionsDamageModifier * actionsPerkModifier)
end



----------------------------------------------------------------------------------------------------
-- Расчет атаки и защиты
----------------------------------------------------------------------------------------------------

function CalcAttackParamsOnServer(unit_server_stats_addr, unit_server_addr,
                                  attack_params_addr, spell_effect_mods) --> none
    local unit       = UnitServer(unit_server_addr)
    local unit_stats = UnitServerStats(unit_server_stats_addr)
    local params     = AttackParams(attack_params_addr)
end

function CalcAttackParamsOnClient(unit_client_stats_addr, unit_client_addr,
                                  attack_params_addr, spell_effect_mods) --> none
    local unit       = UnitServer(unit_client_addr)
    local unit_stats = UnitClientStats(unit_client_stats_addr)
    local params     = AttackParams(attack_params_addr)
end



----------------------------------------------------------------------------------------------------
-- Модификаторы интеллекта
----------------------------------------------------------------------------------------------------

-- Снижение или увеличение затрат запаса сил от номинального на магию
function GetManaBonus(intelligence) --> float
    return 25 / intelligence
end

function GetManaBonusOnServer(unit_server_stats_addr) --> float
    local unit_stats = UnitServerStats(unit_server_stats_addr)
    return GetManaBonus(unit_stats.intelligence.curr)
end

function GetManaBonusOnClient(unit_client_stats_addr) --> float
    local unit_stats = UnitClientStats(unit_client_stats_addr)
    return GetManaBonus(unit_stats.intelligence)
end

-- Влияение интеллекта на получаемый опыт
function GetExpMultiplier(intelligence) --> float
    return 1.04 ^ (intelligence - 25)
end

function GetExpMultiplierOnServer(unit_server_stats_addr) --> float
    local unit_stats = UnitServerStats(unit_server_stats_addr)
    return GetExpMultiplier(unit_stats.intelligence.curr)
end

function GetExpMultiplierOnClient(unit_client_stats_addr) --> float
    local unit_stats = UnitClientStats(unit_client_stats_addr)
    return GetExpMultiplier(unit_stats.intelligence)
end



----------------------------------------------------------------------------------------------------
-- Изменение ценовых коэффициентов у торговцев
----------------------------------------------------------------------------------------------------

function RecalcShopsPriceMultipliers(mults_addr, is_lmp) --> none
    local shops_count = is_lmp and 4 or 5 -- Количество магазинов
    local mults_count = 9                 -- Количество коэффициентов
    local mults = Array(mults_addr, Memory.Type.Float, shops_count * mults_count, false)
	
	--------------------------------------------------
	-- |  Индекс |      Параметр      | По умолчанию |
	--------------------------------------------------
	-- |    0    | Покупка заклинаний |     1.0      |
	-- |    1    | Продажа заклинаний |     0.5      |
	-- |    2    | Сборка заклинания  |     0.2      |
	-- |    3    | Разбор заклинания  |     0.1      |
	--------------------------------------------------
	-- |    4    | Покупка предмета   |     1.0      |
	-- |    5    | Продажа предмета   |     0.5      |
	-- |    6    | Сборка предмета    |     0.2      |
	-- |    7    | Разбор предмета    |     0.1      |
	-- |    8    | Починка предмета   |     0.2      |
	--------------------------------------------------
end



----------------------------------------------------------------------------------------------------
-- Количество опыта, который получит каждый игрок после убийства кем-либо моба
----------------------------------------------------------------------------------------------------

function DistributeLmpExp(exp, playersNum) --> float
    return exp / playersNum
end
