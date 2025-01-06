----------------------------------------------------------------------------------------------------
-- Небольшие вспомогательные классы
----------------------------------------------------------------------------------------------------

require("memory")
require("consts")

print = ConsoleWriteLine

----------------------------------------------------------------------------------------------------
-- PrimaryStat
----------------------------------------------------------------------------------------------------
PrimaryStat = Memory.MakeRefClass()

PrimaryStat:addProps {
    base = Memory.MakeProperty(Memory.Type.Float, 0x00, false),
    curr = Memory.MakeProperty(Memory.Type.Float, 0x04, false)
}

function PrimaryStat:addBase(value) self.base = self.base + value end
function PrimaryStat:subBase(value) self.base = self.base - value end
function PrimaryStat:addCurr(value) self.curr = self.curr + value end
function PrimaryStat:subCurr(value) self.curr = self.curr - value end



----------------------------------------------------------------------------------------------------
-- SecondaryStat
----------------------------------------------------------------------------------------------------
SecondaryStat = Memory.MakeRefClass()

SecondaryStat:addProps {
    curr  = Memory.MakeProperty(Memory.Type.Float, 0x00, false),
    max   = Memory.MakeProperty(Memory.Type.Float, 0x04, false),
    regen = Memory.MakeProperty(Memory.Type.Float, 0x08, false)
}

function PrimaryStat:addCurr(value)  self.curr  = self.curr  + value end
function PrimaryStat:subCurr(value)  self.curr  = self.curr  - value end
function PrimaryStat:addMax(value)   self.max   = self.max   + value end
function PrimaryStat:subMax(value)   self.max   = self.max   - value end
function PrimaryStat:addRegen(value) self.regen = self.regen + value end
function PrimaryStat:subRegen(value) self.regen = self.regen - value end

----------------------------------------------------------------------------------------------------
-- ArmorLayer
----------------------------------------------------------------------------------------------------
ArmorLayer = Memory.MakeRefClass()

ArmorLayer:addProps {
    crippleMask = Memory.MakeProperty(Memory.Type.UInt32, 0x00, false),
    state       = Memory.MakeProperty(Memory.Type.UInt32, 0x3C, false),
    
    absorbtion = function(self) --> Array
        return Array(self.addr + 0x04, Memory.Type.Float, 7, false)
    end,
    resistance = function(self) --> Array
        return Array(self.addr + 0x20, Memory.Type.Float, 7, false)
    end
}



----------------------------------------------------------------------------------------------------
-- HitLocation
----------------------------------------------------------------------------------------------------
HitLocation = Memory.MakeRefClass()

HitLocation:addProps {
    type        = Memory.MakeProperty(Memory.Type.UInt32, 0x00, false),
    crippleMask = Memory.MakeProperty(Memory.Type.UInt32, 0x04, false),
    state       = Memory.MakeProperty(Memory.Type.UInt32, 0xE0, false),
    hp          = Memory.MakeProperty(Memory.Type.Float,  0xE4, false),
    maxHp       = Memory.MakeProperty(Memory.Type.Float,  0xE8, false),
    hpWeight    = Memory.MakeProperty(Memory.Type.Float,  0xEC, false),
    vitality    = Memory.MakeProperty(Memory.Type.Float,  0xF0, false),
}

function HitLocation:layer(index) --> ArmorLayer
    assert(index >= 0 and index <= 2)
    return ArmorLayer:new(self.addr + 0x08 + index * 0x48)
end



----------------------------------------------------------------------------------------------------
-- SpellEffect
----------------------------------------------------------------------------------------------------
SpellEffect = Memory.MakeRefClass()

SpellEffect:addProps {
    id       = Memory.MakeProperty(Memory.Type.UInt8,   0x00, false),
    strength = Memory.MakeProperty(Memory.Type.UInt8,   0x01, false),
    duration = Memory.MakeProperty(Memory.Type.Int32,   0x04, false),
    power    = Memory.MakeProperty(Memory.Type.Float,   0x08, false),
    radius   = Memory.MakeProperty(Memory.Type.Float,   0x0C, false),
    isQuiet  = Memory.MakeProperty(Memory.Type.Boolean, 0x10, false)
}



----------------------------------------------------------------------------------------------------
-- AttackParams
----------------------------------------------------------------------------------------------------
AttackParams = Memory.MakeRefClass()

AttackParams:addProps {
    weaponType   = Memory.MakeProperty(Memory.Type.UInt32, 0x00, false),
    damageType   = Memory.MakeProperty(Memory.Type.UInt32, 0x04, false),
    weaponWeight = Memory.MakeProperty(Memory.Type.Float,  0x08, false),
    damageMin    = Memory.MakeProperty(Memory.Type.Float,  0x0C, false),
    damageMax    = Memory.MakeProperty(Memory.Type.Float,  0x10, false),
    toHitRandom  = Memory.MakeProperty(Memory.Type.Float,  0x14, false),
    attack       = Memory.MakeProperty(Memory.Type.Int32,  0x18, false),
    defense      = Memory.MakeProperty(Memory.Type.Int32,  0x1C, false),
}