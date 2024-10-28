----------------------------------------------------------------------------------------------------
-- Предметы в игре и все, что с ними связано
----------------------------------------------------------------------------------------------------

require("memory")
require("database")

----------------------------------------------------------------------------------------------------
-- Spell
----------------------------------------------------------------------------------------------------
Spell = Memory.MakeRefClass()

Spell:addProps {
    id         = Memory.MakeProperty(Memory.Type.UInt32, 0x0C, false),
    typeId     = Memory.MakeProperty(Memory.Type.UInt32, 0x10, false),
    subtypeId  = Memory.MakeProperty(Memory.Type.UInt16, 0x1A, false),
    speed      = Memory.MakeProperty(Memory.Type.Float,  0x30, false),
    range      = Memory.MakeProperty(Memory.Type.Float,  0x34, false),
    numTargets = Memory.MakeProperty(Memory.Type.UInt32, 0x38, false),
    resolve    = Memory.MakeProperty(Memory.Type.UInt32, 0x3C, false),
    manaCost   = Memory.MakeProperty(Memory.Type.Float,  0x40, false),
    slots      = Memory.MakeProperty(Memory.Type.UInt32, 0x44, false),
    price      = Memory.MakeProperty(Memory.Type.Float,  0x48, false),
    actions    = Memory.MakeProperty(Memory.Type.UInt32, 0x4C, false),
    complexity = Memory.MakeProperty(Memory.Type.UInt32, 0x50, false),
    
    runeIds = function(self) --> int[8]
        return Array(self.addr + 0x54, Memory.Type.UInt8, 8, false)
    end,
    effect = function(self) --> SpellEffect
        return SpellEffect(self.addr + 0x1C)
    end
}



----------------------------------------------------------------------------------------------------
-- Base item (abstract)
----------------------------------------------------------------------------------------------------
BaseItem = Memory.MakeRefClass()

BaseItem:addProps {
    id            = Memory.MakeProperty(Memory.Type.UInt32, 0x0C, false),
    typeId        = Memory.MakeProperty(Memory.Type.UInt32, 0x10, false),
    materialId    = Memory.MakeProperty(Memory.Type.UInt16, 0x18, false),
    prototypeId   = Memory.MakeProperty(Memory.Type.UInt16, 0x1A, false),
    durability    = Memory.MakeProperty(Memory.Type.Float,  0x20, false),
    durabilityMax = Memory.MakeProperty(Memory.Type.Float,  0x24, false),
    mp            = Memory.MakeProperty(Memory.Type.Float,  0x28, false),
    mpMax         = Memory.MakeProperty(Memory.Type.Float,  0x2C, false),
    regenRate     = Memory.MakeProperty(Memory.Type.Float,  0x30, false),
    weight        = Memory.MakeProperty(Memory.Type.Float,  0x34, false),
    price         = Memory.MakeProperty(Memory.Type.Float,  0x38, false),
    size          = Memory.MakeProperty(Memory.Type.Float,  0x3C, false),
    numRunes      = Memory.MakeProperty(Memory.Type.UInt32, 0x40, false),
    count         = Memory.MakeProperty(Memory.Type.Int32,  0x48, false),
    
    spell = function(self) --> Spell or nil
        local addr = Memory.Read(self.addr + 0x44, Memory.Type.UInt32)
        if addr == 0 then
            return nil
        end
        return Spell(addr)
    end,

    prototypeAddr = function(self) --> int
        return GetItemShapeEntry(self.addr)
    end
}



----------------------------------------------------------------------------------------------------
-- Weapon
----------------------------------------------------------------------------------------------------
ItemWeapon = BaseItem:extend()

ItemWeapon:addProps {
    damageMin   = Memory.MakeProperty(Memory.Type.Float,  0x50, false),
    damageRnd   = Memory.MakeProperty(Memory.Type.Float,  0x54, false),
    range       = Memory.MakeProperty(Memory.Type.Float,  0x58, false),
    proficiency = Memory.MakeProperty(Memory.Type.UInt32, 0x4C, false),
    
    damageMax = function(self) --> float
        return self.damageMin + self.damageRnd
    end,
    actions = function(self) --> int
        return Memory.Read(self.prototypeAddr + 0x6C, Memory.Type.UInt32)
    end,
    prototype = function(self) --> ItemsDatabase.WeaponsTableRecord
        return ItemsDatabase.WeaponsTableRecord(self.prototypeAddr)
    end
}



----------------------------------------------------------------------------------------------------
-- Armor
----------------------------------------------------------------------------------------------------
ItemArmor = BaseItem:extend()

ItemArmor:addProps {
    absorbtions = function(self) --> float[7]
        return Array(self.addr + 0x4C, Memory.Type.Float, 7, false)
    end,
    resistances = function(self) --> float[7]
        return Array(self.addr + 0x68, Memory.Type.Float, 7, false)
    end,
    crippleMasks = function(self) --> int[2]
        return Array(self.addr + 0x84, Memory.Type.UInt32, 2, false)
    end,
    prototype = function(self) --> ItemsDatabase.WeaponsTableRecord
        return ItemsDatabase.ArmorsTableRecord(self.prototypeAddr)
    end
}



----------------------------------------------------------------------------------------------------
-- Spell (container)
----------------------------------------------------------------------------------------------------
ItemSpell = BaseItem:extend()



----------------------------------------------------------------------------------------------------
-- Quick (wand/potion)
----------------------------------------------------------------------------------------------------
ItemQuick = BaseItem:extend()

ItemQuick:addProps {
    skill = Memory.MakeProperty(Memory.Type.UInt32, 0x4C, false),
    prototype = function(self) --> ItemsDatabase.WeaponsTableRecord
        return ItemsDatabase.QuicksTableRecord(self.prototypeAddr)
    end
}
