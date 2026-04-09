----------------------------------------------------------------------------------------------------
-- Классы для работы с таблицами .*db файлов из database(lmp).res
----------------------------------------------------------------------------------------------------

require("memory")

----------------------------------------------------------------------------------------------------
-- Умения и навыки (perks.pdb)
----------------------------------------------------------------------------------------------------
PerksDatabase = { }

-- Таблица умений (PerksTable)
PerksDatabase.PerksTable        = { }
PerksDatabase.PerksTableRecord = Memory.MakeRefClass()
PerksDatabase.PerksTableRecord:addProps {
    name          = Memory.MakeProperty(Memory.Type.String,  0x00, true),
    code          = Memory.MakeProperty(Memory.Type.String,  0x04, true),
    tti           = Memory.MakeProperty(Memory.Type.UInt32,  0x08, true),
    perkRequired  = Memory.MakeProperty(Memory.Type.String,  0x0C, true),
    perkExclusive = Memory.MakeProperty(Memory.Type.Int32,   0x10, true),
    skillRequired = Memory.MakeProperty(Memory.Type.String,  0x14, true),
    autoSkill     = Memory.MakeProperty(Memory.Type.Int32,   0x18, true),
    manualSkill   = Memory.MakeProperty(Memory.Type.Int32,   0x1C, true),
    skillLevel    = Memory.MakeProperty(Memory.Type.UInt32,  0x20, true),
    strength      = Memory.MakeProperty(Memory.Type.Float,   0x24, true),
    dexterity     = Memory.MakeProperty(Memory.Type.Float,   0x28, true),
    intelligence  = Memory.MakeProperty(Memory.Type.Float,   0x2C, true),
    cost          = Memory.MakeProperty(Memory.Type.UInt32,  0x30, true),
    modifier      = Memory.MakeProperty(Memory.Type.UInt32,  0x34, true),
    mult          = Memory.MakeProperty(Memory.Type.UInt32,  0x38, true),
    add           = Memory.MakeProperty(Memory.Type.UInt32,  0x3C, true),
    active        = Memory.MakeProperty(Memory.Type.Boolean, 0x40, true)
}

-- Вычисление индекса (номера строки) умения в базе данных
function PerksDatabase.PerksTable.getRecordIndex(perk) --> int
    return GetPerkIndex(perk.addr)
end

-- Возвращает умение по его номеру индекса из базы данных. Nil если такого индекса в таблице нет
function PerksDatabase.PerksTable.getRecord(index) --> PerksDatabase.PerksTableRecord
    local addr = GetPerkRecord(index)
    if addr == 0 then return nil end
    return PerksDatabase.PerksTableRecord(addr)
end



----------------------------------------------------------------------------------------------------
-- Предметы (items.idb)
----------------------------------------------------------------------------------------------------

----------------------------------------------------------------------------------------------------
-- Общие поля для всех предметов (GeneralRecord)
----------------------------------------------------------------------------------------------------
ItemsDatabase = { }
ItemsDatabase.GeneralRecord = Memory.MakeRefClass()
ItemsDatabase.GeneralRecord:addProps {
    name              = Memory.MakeProperty(Memory.Type.String,  0x04, true),
    type              = Memory.MakeProperty(Memory.Type.String,  0x08, true),
    typeId            = Memory.MakeProperty(Memory.Type.UInt32,  0x0C, true),
    materialType      = Memory.MakeProperty(Memory.Type.String,  0x10, true),
    materialTypeIndex = Memory.MakeProperty(Memory.Type.UInt32,  0x14, true),
    tti1              = Memory.MakeProperty(Memory.Type.UInt32,  0x18, true),
    tti2              = Memory.MakeProperty(Memory.Type.UInt32,  0x1C, true),
    price             = Memory.MakeProperty(Memory.Type.Float,   0x34, true),
    weight            = Memory.MakeProperty(Memory.Type.Float,   0x38, true),
    size              = Memory.MakeProperty(Memory.Type.Float,   0x3C, true),
    manaCapacity      = Memory.MakeProperty(Memory.Type.Float,   0x40, true),
    spellSlots        = Memory.MakeProperty(Memory.Type.UInt32,  0x44, true),
    durability        = Memory.MakeProperty(Memory.Type.Float,   0x48, true),
    components        = Memory.MakeProperty(Memory.Type.UInt32,  0x4C, true),
    availableInShop   = Memory.MakeProperty(Memory.Type.UInt32,  0x64, true),
    deconstructable   = Memory.MakeProperty(Memory.Type.Boolean, 0x68, true)

    -- Пока что класс массива из строк не реализован
    -- figures = Memory.MakeProperty(Memory.Type.StringArray, 0x20, true),
    -- zones   = Memory.MakeProperty(Memory.Type.StringArray, 0x50, true)
}



----------------------------------------------------------------------------------------------------
-- Таблица оружия (WeaponsTable)
----------------------------------------------------------------------------------------------------
ItemsDatabase.WeaponsTable        = { }
ItemsDatabase.WeaponsTableRecord = ItemsDatabase.GeneralRecord:extend()
ItemsDatabase.WeaponsTableRecord:addProps {
    actions      = Memory.MakeProperty(Memory.Type.UInt32, 0x6C, true),
    attackBonus  = Memory.MakeProperty(Memory.Type.Float,  0x84, true),
    defenceBonus = Memory.MakeProperty(Memory.Type.Float,  0x88, true),
    range        = Memory.MakeProperty(Memory.Type.Float,  0x8C, true),
    damageMin    = Memory.MakeProperty(Memory.Type.Float,  0x90, true),
    damageRandom = Memory.MakeProperty(Memory.Type.Float,  0x94, true),
    
    damageMax = function(self) --> float
        return self.damageMin + self.damageRandom
    end,
    statMods = function(self) --> int[2]
        return Array(self.addr + 0x70, Memory.Type.UInt32, 2, true)
    end,
    manualSkillMods = function(self) --> int[3]
        return Array(self.addr + 0x78, Memory.Type.UInt8, 3, true)
    end,
    autoSkillMods = function(self) --> int[8]
        return Array(self.addr + 0x7B, Memory.Type.UInt8, 8, true)
    end,
    damageDistributions = function(self) --> float[7]
        return Array(self.addr + 0x98, Memory.Type.Float, 7, true)
    end
}

function ItemsDatabase.WeaponsTable.getRecord(index) --> ItemsDatabase.WeaponsTableRecord
    local addr = GetWeaponRecord(index)
    if addr == 0 then return nil end
    return ItemsDatabase.WeaponsTableRecord(addr)
end



----------------------------------------------------------------------------------------------------
-- Таблица брони (ArmorsTable)
----------------------------------------------------------------------------------------------------
ItemsDatabase.ArmorsTable = { }
ItemsDatabase.ArmorsTableAbsorbtion = Memory.MakeRefClass()
ItemsDatabase.ArmorsTableAbsorbtion:addProps {
    absorbtion  = Memory.MakeProperty(Memory.Type.Float,  0x00, true),
    crippleMask = Memory.MakeProperty(Memory.Type.UInt32, 0x20, true),
    distributions = function(self) --> float[7]
        return Array(self.addr + 0x04, Memory.Type.Float, 7, true)
    end
}

ItemsDatabase.ArmorsTableRecord = ItemsDatabase.GeneralRecord:extend()
ItemsDatabase.ArmorsTableRecord:addProps {
    applyWounds = Memory.MakeProperty(Memory.Type.Boolean, 0xB4, true),
    wearOrder   = Memory.MakeProperty(Memory.Type.UInt32,  0xB8, true),

    main = function(self) --> ItemsDatabase.ArmorsTableAbsorbtion
        return ItemsDatabase.ArmorsTableAbsorbtion(self.addr + 0x6C)
    end,
    secondary = function(self) --> ItemsDatabase.ArmorsTableAbsorbtion
        return ItemsDatabase.ArmorsTableAbsorbtion(self.addr + 0x90)
    end,
    manualSkillMod = function(self) --> int[3]
        return Array(self.addr + 0xBC, Memory.Type.UInt8, 3, true)
    end,
    autoSkillMod = function(self) --> int[8]
        return Array(self.addr + 0xBF, Memory.Type.UInt8, 8, true)
    end
}

function ItemsDatabase.ArmorsTable.getRecord(index) --> ItemsDatabase.ArmorsTableRecord
    local addr = GetArmorRecord(index)
    if addr == 0 then return nil end
    return ItemsDatabase.ArmorsTableRecord(addr)
end



----------------------------------------------------------------------------------------------------
-- Таблица материалов (MaterialsTable)
----------------------------------------------------------------------------------------------------
ItemsDatabase.MaterialsTable = { }
ItemsDatabase.MaterialsTableRecord = Memory.MakeRefClass()
ItemsDatabase.MaterialsTableRecord:addProps {
    name            = Memory.MakeProperty(Memory.Type.String, 0x00, true),
    type            = Memory.MakeProperty(Memory.Type.String, 0x04, true),
    codeName        = Memory.MakeProperty(Memory.Type.String, 0x08, true),
    typeIndex       = Memory.MakeProperty(Memory.Type.UInt32, 0x0C, true),
    price           = Memory.MakeProperty(Memory.Type.Float,  0x10, true),
    weight          = Memory.MakeProperty(Memory.Type.Float,  0x14, true),
    manaCapacity    = Memory.MakeProperty(Memory.Type.Float,  0x18, true),
    slots           = Memory.MakeProperty(Memory.Type.UInt32, 0x1C, true),
    durability      = Memory.MakeProperty(Memory.Type.Float,  0x20, true),
    skill           = Memory.MakeProperty(Memory.Type.UInt32, 0x24, true),
    damageMod       = Memory.MakeProperty(Memory.Type.Float,  0x28, true),
    crippleMask     = Memory.MakeProperty(Memory.Type.UInt32, 0x48, true),
    availableInShop = Memory.MakeProperty(Memory.Type.UInt32, 0x4C, true),

    absorbtionMultipliers = function(self) --> float[7]
        return Array(self.addr + 0x2C, Memory.Type.Float, 7, true)
    end
}

function ItemsDatabase.MaterialsTable.getRecord(index) --> ItemsDatabase.MaterialsTableRecord
    local addr = GetMaterialRecord(index)
    if addr == 0 then return nil end
    return ItemsDatabase.MaterialsTableRecord(addr)
end



----------------------------------------------------------------------------------------------------
-- Быстрые предметы/жезлы (QuicksTable)
----------------------------------------------------------------------------------------------------
ItemsDatabase.QuicksTable = { }

-- Типы быстрых предметов
ItemsDatabase.QuicksTableType = {
    DangerSensor = 0,
    Persuader    = 1,
    Trap         = 2,
    Picklock     = 3,
    Crowbar      = 4,
    Wand         = 5,
    Key          = 6,
    Plug         = 7,
    Scroll       = 8
}

ItemsDatabase.QuicksTableRecord = ItemsDatabase.GeneralRecord:extend()
ItemsDatabase.QuicksTableRecord:addProps {
    quickType  = Memory.MakeProperty(Memory.Type.UInt32, 0x6C, true), -- QuicksTableType
    skill      = Memory.MakeProperty(Memory.Type.UInt32, 0x70, true),
    damage     = Memory.MakeProperty(Memory.Type.Float , 0x74, true),
    damageType = Memory.MakeProperty(Memory.Type.UInt32, 0x78, true), -- DamageType
    spell      = Memory.MakeProperty(Memory.Type.String, 0x7C, true),

    manualSkillMods = function(self) --> int[3]
        return Array(self.addr + 0x80, Memory.Type.UInt8, 3, true)
    end,
    autoSkillMods = function(self) --> int[8]
        return Array(self.addr + 0x83, Memory.Type.UInt8, 8, true)
    end
}

function ItemsDatabase.QuicksTable.getRecord(index) --> ItemsDatabase.MaterialsTableRecord
    local addr = GetQuickRecord(index)
    if addr == 0 then return nil end
    return ItemsDatabase.QuicksTableRecord(addr)
end
