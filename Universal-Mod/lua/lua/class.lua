----------------------------------------------------------------------------------------------------
-- Реализация ООП на Lua
----------------------------------------------------------------------------------------------------

-- Вспомогательные функции для BaseClass
local function NewIndex(self, key, value) --> table
    local meta = getmetatable(self)
    local props = rawget(meta, "_")
    if props ~= nil then
        local prop = props[key]
        if prop ~= nil and type(prop) == "function" then
            return prop(self, value)
        end
    end
    if value ~= nil then
        rawset(self, key, value)
    end
    local result = rawget(self, key)
    if result ~= nil then
        return result
    end
    return meta[key]
end

local function Index(self, key) --> table
    return NewIndex(self, key, nil)
end

-- Объединение двух таблиц в одну
function MergeTables(table1, table2) --> table
    local result = { }
    if table1 then
        for k, v in pairs(table1) do
            result[k] = v
        end
    end
    if table2 then
        for k, v in pairs(table2) do
            result[k] = v
        end
    end
    return result
end

-- Выбрасывает исключение с указанным текстом
function Raise(message) --> none
    assert(false, message)
end

-- Вывод таблицы в JSON-like формате
function Dump(obj, level) --> string
    local level = level or -1
    if type(obj) == "table" then
        local s = "{\n"
        level = level + 1
        for k,v in pairs(obj) do
            for i = 0, level do s = s .. "    " end
            if type(k) ~= "number" then k = '"'..k..'"' end
            s = s .. k .. " = " .. Dump(v, level) .. "\n"
        end
        level = level - 1
        for i = 0, level do s = s .. "    " end
        return s .. "}"
    else
        return tostring(obj)
    end
end

-- Базовый класс, от которого наследуются все остальные
BaseClass = {
    _          = { },
    __index    = Index,
    __newindex = NewIndex,
    __call     = function(self, ...)
        return self:new(...)
    end
}

-- Создание и инициализация экземпляра класса
function BaseClass:new(addr) --> BaseClass
    assert(addr ~= 0, "Attempt to create BaseClass from NULL pointer")
    local obj = { addr = addr }
    setmetatable(obj, self)
    return obj
end

-- Наследование от класса
function BaseClass:extend(props) --> table
    local class = {
        _          = MergeTables(self._, props),
        __index    = Index,
        __newindex = NewIndex,
        __call     = function(self, ...)
            return self:new(...)
        end
    }
    return setmetatable(class, self)
end

-- Добавление property вне конструктора класса
function BaseClass:addProps(props) --> none
    self._ = MergeTables(self._, props)
end
