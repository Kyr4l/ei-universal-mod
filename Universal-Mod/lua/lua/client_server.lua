----------------------------------------------------------------------------------------------------
-- Игровые структуры, хранящие характеристики игрока на стороне клиента или сервера
----------------------------------------------------------------------------------------------------

require("helpers")
require("items")
require("database")

----------------------------------------------------------------------------------------------------
-- BaseUnit (abstract)
----------------------------------------------------------------------------------------------------
BaseUnit = Memory.MakeRefClass()

BaseUnit:addProps {
    id              = Memory.MakeProperty(Memory.Type.UInt32,  0x00C, true),
    name            = Memory.MakeProperty(Memory.Type.String,  0x238, true),
    playerNum       = Memory.MakeProperty(Memory.Type.UInt32,  0x250, true),
    pose            = Memory.MakeProperty(Memory.Type.UInt32,  0x264, true),
    currWeaponIndex = Memory.MakeProperty(Memory.Type.UInt32,  0x26C, true),
    isDead          = Memory.MakeProperty(Memory.Type.Boolean, 0x274, true),
    isMoving        = Memory.MakeProperty(Memory.Type.Boolean, 0x278, true),

    -- Для игрока это значение переключателя снизу слева (атака/защита). Для мобов неизвестно
    isAggressive = Memory.MakeProperty(Memory.Type.Boolean, 0x438, true),

    classId = function(self) --> int
        return GetClassId(self.addr)
    end,
    itemsWeight = function(self) --> float
        return GetItemsWeight(self.addr)
    end,
    isPlayer = function(self) --> bool
        local first_id = 1000000000
        return (self.id >= first_id) and (self.id < 2 * first_id)
    end,
    unitSpecificAddr = function(self) --> int
        return self.addr + 0x240
    end,
    currWeapon = function(self) --> Weapon or nil
        return self:weapon(self.currWeaponIndex)
    end
}

-- spell: SpellType
function BaseUnit:spellEffect(spell) --> float
    return GetEffectValue(self.addr, spell)
end

function BaseUnit:weapon(index) --> ItemWeapon or nil
    local addr = GetWeapon(self.addr, index)
    if addr ~= 0 then
        return ItemWeapon(addr)
    end
    return nil
end

function BaseUnit:armor(index) --> ItemArmor or nil
    local addr = GetArmor(self.addr, index)
    if addr ~= 0 then
        return ItemArmor(addr)
    end
    return nil
end

function BaseUnit:quickItem(index)  --> ItemQuick or nil
    local addr = GetQuick(self.addr, index)
    if addr ~= 0 then
        return ItemQuick(addr)
    end
    return nil
end

function BaseUnit:spell(index) --> ItemSpell or nil
    local addr = GetSpell(self.addr, index)
    if addr ~= 0 then
        return ItemSpell(addr)
    end
    return nil
end

UnitServer = BaseUnit:extend()
UnitClient = BaseUnit:extend()



----------------------------------------------------------------------------------------------------
-- UnitServerCommonStats
----------------------------------------------------------------------------------------------------
UnitServerCommonStats = Memory.MakeRefClass()

UnitServerCommonStats:addProps {
    flags        = Memory.MakeProperty(Memory.Type.UInt32, 0x00, false),
    regenCounter = Memory.MakeProperty(Memory.Type.Int32,  0x04, false),
    hp = function(self) --> SecondaryStat
        return SecondaryStat(self.addr + 0x08)
    end,
    mp = function(self) --> SecondaryStat
        return SecondaryStat(self.addr + 0x14)
    end
}

function UnitServerCommonStats:absorbtion(index) --> PrimaryStat
    assert(index >= 0 and index < 7)
    return PrimaryStat(self.addr + 0x20 + index * 0x08)
end

-- value: float
function UnitServerCommonStats:regenMp(value) --> none
    RegenMp(self.addr, value)
end



----------------------------------------------------------------------------------------------------
-- UnitServerStats
----------------------------------------------------------------------------------------------------
UnitServerStats = Memory.MakeRefClass()

UnitServerStats:addProps {
    flags1                = Memory.MakeProperty(Memory.Type.UInt32, 0x000, false),
    flags2                = Memory.MakeProperty(Memory.Type.UInt32, 0x6F8, false),
    experience            = Memory.MakeProperty(Memory.Type.Float,  0x004, false),
    spentExperience       = Memory.MakeProperty(Memory.Type.Float,  0x008, false),
    visionArc             = Memory.MakeProperty(Memory.Type.Float,  0x044, false),
    currEncumbrance       = Memory.MakeProperty(Memory.Type.Float,  0x050, false),
    maxEncumbrance        = Memory.MakeProperty(Memory.Type.Float,  0x054, false),
    locomotion            = Memory.MakeProperty(Memory.Type.UInt32, 0x060, false),
    baseHp                = Memory.MakeProperty(Memory.Type.Float,  0x6FC, false),
    expPenalty            = Memory.MakeProperty(Memory.Type.Float,  0x700, false),
    moveDamageModifier    = Memory.MakeProperty(Memory.Type.Float,  0x058, false),
    actionsDamageModifier = Memory.MakeProperty(Memory.Type.Float,  0x05C, false),

    freeExperience = function(self) --> float
        return self.experience - self.spentExperience - self.expPenalty
    end,

    nawRange        = Memory.MakeProperty(Memory.Type.Float,  0x70, false),
    nawToHit        = Memory.MakeProperty(Memory.Type.Float,  0x74, false),
    nawToHitRandom  = Memory.MakeProperty(Memory.Type.Float,  0x78, false),
    nawParry        = Memory.MakeProperty(Memory.Type.Float,  0x7C, false),
    nawWeight       = Memory.MakeProperty(Memory.Type.Float,  0x80, false),
    nawDamageMin    = Memory.MakeProperty(Memory.Type.Float,  0x84, false),
    nawDamageRandom = Memory.MakeProperty(Memory.Type.Float,  0x88, false),
    nawActions      = Memory.MakeProperty(Memory.Type.Float,  0x8C, false),
    nawType         = Memory.MakeProperty(Memory.Type.UInt32, 0xAC, false),

    nawAttacks = function(self) --> float[7]
        return Array(self.addr + 0x90, Memory.Type.Float, 7,  false)
    end,

    nawDamageMax = function(self) --> float
        return self.nawDamageMin + self.nawDamageRandom
    end,

    strength     = function(self) return PrimaryStat(self.addr + 0x0C) end, --> PrimaryStat
    dexterity    = function(self) return PrimaryStat(self.addr + 0x14) end, --> PrimaryStat
    intelligence = function(self) return PrimaryStat(self.addr + 0x1C) end, --> PrimaryStat
    move         = function(self) return PrimaryStat(self.addr + 0x24) end, --> PrimaryStat
    actions      = function(self) return PrimaryStat(self.addr + 0x2C) end, --> PrimaryStat

    speeds       = function(self) return Array(self.addr + 0x034, Memory.Type.Float, 4,  false) end, --> float[4]
    manualSkills = function(self) return Array(self.addr + 0x6C8, Memory.Type.UInt8, 3,  false) end, --> uint8_t[3]
    autoSkills   = function(self) return Array(self.addr + 0x6CB, Memory.Type.UInt8, 8,  false) end, --> uint8_t[8]
    perks        = function(self) return Array(self.addr + 0x6D3, Memory.Type.UInt8, 36, false) end, --> uint8_t[36]

    isServer = function(self) --> bool
        return true
    end
}

function UnitServerStats:sense(index) --> PrimaryStat
    assert(index >= 0 and index < 6)
    return PrimaryStat(self.addr + 0xB0 + index * 0x08)
end

function UnitServerStats:percept(index) --> PrimaryStat
    assert(index >= 0 and index < 6)
    return PrimaryStat(self.addr + 0xE0 + index * 0x08)
end

function UnitServerStats:hitLocation(index) --> HitLocation
    assert(index >= 0 and index < 6)
    return HitLocation(self.addr + 0x110 + index * 0xF4)
end

-- perk: PerkType
function UnitServerStats:perkModifier(perk) --> float
    assert(perk >= 0 and perk < 36)
    return GetPerkModifierForServer(self.addr, perk)
end

-- value: float
function UnitServerStats:heal(common_server_stats_ptr, value) --> none
    Heal(self.addr, common_server_stats_ptr, value)
end

-- max_hit_points: float
function UnitServerStats:distributeHp(max_hit_points) --> none
    DistributeHp(self.addr, max_hit_points)
end

-- max_hit_points: float
function UnitServerStats:collectHp(max_hit_points) --> float
    return CollectHp(self.addr, max_hit_points)
end



----------------------------------------------------------------------------------------------------
-- UnitClientStats
----------------------------------------------------------------------------------------------------
UnitClientStats = Memory.MakeRefClass()

UnitClientStats:addProps {
    experience     = Memory.MakeProperty(Memory.Type.UInt32, 0x04, false),
    freeExperience = Memory.MakeProperty(Memory.Type.Int32,  0x08, false),
    strength       = Memory.MakeProperty(Memory.Type.UInt8,  0x0C, false),
    dexterity      = Memory.MakeProperty(Memory.Type.UInt8,  0x0D, false),
    intelligence   = Memory.MakeProperty(Memory.Type.UInt8,  0x0E, false),
    locomotion     = Memory.MakeProperty(Memory.Type.UInt8,  0x0F, false),
    movePoints     = Memory.MakeProperty(Memory.Type.UInt8,  0x14, false),
    actions        = Memory.MakeProperty(Memory.Type.UInt8,  0x15, false),
    encumbrance    = Memory.MakeProperty(Memory.Type.UInt16, 0x16, false),
    attackerProto  = Memory.MakeProperty(Memory.Type.UInt16, 0x20, false),

    speeds = function(self) --> int[4]
        return Array(self.addr + 0x10, Memory.Type.UInt8, 4,  false)
    end,
    senses = function(self) --> int[6]
        return Array(self.addr + 0x22, Memory.Type.UInt8, 6,  false)
    end,
    percepts = function(self) --> int[6]
        return Array(self.addr + 0x28, Memory.Type.UInt8, 6,  false)
    end,
    manualSkills = function(self) --> int[3]
        return Array(self.addr + 0xD8, Memory.Type.UInt8, 3,  false)
    end,
    autoSkills = function(self) --> int[8]
        return Array(self.addr + 0xDB, Memory.Type.UInt8, 8,  false)
    end,
    perks = function(self) --> int[36]
        return Array(self.addr + 0xE3, Memory.Type.UInt8, 36, false)
    end,
    isServer = function(self) --> bool
        return false
    end
}

-- perk: PerkType
function UnitClientStats:perkModifier(perk) --> float
    assert(perk >= 0 and perk < 36)
    return GetPerkModifierForClient(self.addr, perk)
end
