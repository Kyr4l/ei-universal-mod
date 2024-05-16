----------------------------------------------------------------------------------------------------
-- Низкоуровневая работа с памятью игры
----------------------------------------------------------------------------------------------------

require("class")

Memory = { }

-- Поддерживаемые типы (из C/C++)
Memory.Type = {
    UInt8   = 0, -- uint8_t
    UInt16  = 1, -- uint16_t
    UInt32  = 2, -- uint32_t
    Int8    = 3, -- int8_t
    Int16   = 4, -- int16_t
    Int32   = 5, -- int32_t
    Float   = 6, -- float
    Double  = 7, -- double
    Boolean = 8, -- bool
    String  = 9  -- const char *
}



-- Размеры типов (в байтах)
local size_of_switch_table = {
    [Memory.Type.UInt8]   = 1,
    [Memory.Type.UInt16]  = 2,
    [Memory.Type.UInt32]  = 4,
    [Memory.Type.Int8]    = 1,
    [Memory.Type.Int16]   = 2,
    [Memory.Type.Int32]   = 4,
    [Memory.Type.Float]   = 4,
    [Memory.Type.Double]  = 8,
    [Memory.Type.Boolean] = 4
}
function Memory.sizeOf(mem_type) --> int
    local result = size_of_switch_table[mem_type]
    return result and result or Raise("Invalid type")
end



-- Чтение памяти
local read_switch_table = {
    [Memory.Type.UInt8]   = UInt8FromMem,
    [Memory.Type.UInt16]  = UInt16FromMem,
    [Memory.Type.UInt32]  = UInt32FromMem,
    [Memory.Type.Int8]    = Int8FromMem,
    [Memory.Type.Int16]   = Int16FromMem,
    [Memory.Type.Int32]   = Int32FromMem,
    [Memory.Type.Float]   = FloatFromMem,
    [Memory.Type.Double]  = DoubleFromMem,
    [Memory.Type.String]  = StrFromMem,
    [Memory.Type.Boolean] = function(addr)
        return UInt32FromMem(addr) == 1
    end
}
function Memory.Read(addr, mem_type) --> any
    local result = read_switch_table[mem_type]
    if result == nil then Raise("Invalid type") end
    return result(addr)
end



-- Запись в память
local write_switch_table = {
    [Memory.Type.UInt8]   = UInt8ToMem,
    [Memory.Type.UInt16]  = UInt16ToMem,
    [Memory.Type.UInt32]  = UInt32ToMem,
    [Memory.Type.Int8]    = Int8ToMem,
    [Memory.Type.Int16]   = Int16ToMem,
    [Memory.Type.Int32]   = Int32ToMem,
    [Memory.Type.Float]   = FloatToMem,
    [Memory.Type.Double]  = DoubleToMem,
    -- Изменение строк пока что не реализовано
    -- [Memory.Type.String]  = StrToMem,
    [Memory.Type.Boolean] = function(addr, value)
        UInt32ToMem(addr and 1 or 0, value)
    end
}
function Memory.Write(addr, mem_type, value) --> none
    local result = write_switch_table[mem_type]
    if result == nil then Raise("Invalid type") end
    return result(addr, value)
end



----------------------------------------------------------------------------------------------------
-- Базовый класс, данные которого находятся в памяти игры, а доступ к ним осуществляется через
-- указатель на первый элемент объекта
----------------------------------------------------------------------------------------------------

GameObject = BaseClass:extend()

function GameObject:new(addr) --> GameObject
    local obj = BaseClass:new(addr)
    setmetatable(obj, self)
    return obj
end

-- Создание простого класса, наследующегося от GameObject
function Memory.MakeRefClass() --> table
    local class = GameObject:extend()

    class.new = function(self, addr) --> GameObject
        local obj = GameObject:new(addr)
        setmetatable(obj, self)
        return obj
    end

    return class
end

-- Создание property для класса, способной читать и, если
-- read_only равен false или nil, записывать в память
function Memory.MakeProperty(mem_type, offset, read_only) --> table
    return function (self, value)
        if read_only ~= nil and read_only == true and value ~= nil then
            Raise("Attempt to call property which was defined as read only")
        end
        if value ~= nil then
            Memory.Write(self.addr + offset, mem_type, value)
        else
            return Memory.Read(self.addr + offset, mem_type)
        end
    end
end



----------------------------------------------------------------------------------------------------
-- Враппер для массивов C/C++
----------------------------------------------------------------------------------------------------
Array = GameObject:extend()

function Array:new(addr, mem_type, size, read_only) --> Array
     -- 8 aka String не разрешен (пока что)
    assert(mem_type >= 0 and mem_type <= 7, "Wrong array items type")
    assert(size > 0, "Attempt to create array with size lower or equal 0")
 
    local obj = GameObject:new(addr)
    obj.mem_type  = mem_type
    obj.size      = size
    obj.read_only = read_only ~= nil and read_only == true or false
    setmetatable(obj, self)
    return obj
end

-- Чтение значения
function Array:value(index) --> any
    if index < 0 or index >= self.size then
        Raise(tostring(index) .. " not in range [0.." .. tostring(self.size - 1))
    end
    return Memory.Read(self.addr + index * Memory.sizeOf(self.mem_type), self.mem_type)
end

-- Запись значения
function Array:set(index, value) --> none
    assert(not self.read_only, "Attempt to modify read only defined array")
    if index < 0 or index >= self.size then
        Raise(tostring(index) .. " not in range [0.." .. tostring(self.size - 1))
    end
    Memory.Write(self.addr + index * Memory.sizeOf(self.mem_type), self.mem_type, value)
end
