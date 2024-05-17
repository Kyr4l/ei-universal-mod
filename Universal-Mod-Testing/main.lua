----------------------------------------------------------------------------------------------------
-- Основной исполняемый файл
----------------------------------------------------------------------------------------------------

require("default")

function CalcRunningManaCost(unit_server_stats_addr, unit_server_addr, common_server_stats_addr) --> none
    local unit_stats = UnitServerStats(unit_server_stats_addr)
    local unit = UnitServer(unit_server_addr)
    local common_stats = UnitServerCommonStats(common_server_stats_addr)
    local mp = common_stats.mp

    mp.curr = mp.curr - mp.max * (1 / 375)
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
