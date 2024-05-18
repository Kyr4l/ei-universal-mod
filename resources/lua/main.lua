----------------------------------------------------------------------------------------------------
-- Основной исполняемый файл
----------------------------------------------------------------------------------------------------

require("default")

-- decrease the mana cost of sprinting
function CalcRunningManaCost(unit_server_stats_addr, unit_server_addr, common_server_stats_addr) --> none
    local unit_stats = UnitServerStats(unit_server_stats_addr)
    local unit = UnitServer(unit_server_addr)
    local common_stats = UnitServerCommonStats(common_server_stats_addr)
    local mp = common_stats.mp

    mp.curr = mp.curr - mp.max * (1 / 450)
end

-- fix the perma regen bug
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

-- do not divide xp
function DistributeLmpExp(exp, playersNum) --> float
    return exp
end





-- Здесь можно заменить следующие функции (см. lua/default.lua для примера реализации):
-- 1) GetModifiedDamageOnClient
-- 2) GetModifiedDamageOnServer
-- 3) CalcBaseHealth
-- 4) CalcBaseMana
-- 5) GetModifiedHealth
-- 6) RegenerateHpAndMp
-- 7) GetSkillPriceDiff
-- 8) GetPerkPrice
-- 9) CalcRunningManaCost
-- 10) RecalcUnitStats
-- 11) RecalcInternalHeroStats
-- какие-то еще из default.lua...
--
-- Остальные являются вспомогательными и игнорируются аддоном, но используются для вычисления
-- значений функций выше. Так что их можно удалять, объединять и так далее. В default.lua есть две
-- вспомогательные функции отмеченные как "Внимание: не меняйте алгоритм этой функции". Это
-- означает, что данные функции окажутся в скором времени в списке выше, но пока что еще нет.
-- Единственное что в них можно менять это внутренние константы (которые обычно игрой берутся из
-- ai.reg). В таком случае важно, чтобы эти константы совпадали с тем, что в ai.reg
