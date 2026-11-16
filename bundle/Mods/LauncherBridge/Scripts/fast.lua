-- Fast lookups for the local player controller and world.
-- FindAllOf goes through every object in the game. In a big game (Hogwarts Legacy has about two million objects)
-- one call can take tens of milliseconds, so doing it every frame or every second made the game stutter.
-- Here the controller comes straight from the engine: GameEngine -> GameInstance -> LocalPlayers[1] -> PlayerController.
local UEHelpers = require("UEHelpers")
local M = {}

local function Valid(o)
    if o == nil then return false end
    local ok, v = pcall(function() return o:IsValid() end)
    return ok and v == true
end
M.Valid = Valid

local slowGetPC = UEHelpers.GetPlayerController    -- UE4SS's version (searches every object)
local engine = nil
local fallbackPC, fallbackAt = nil, -100

local function Engine()
    if Valid(engine) then return engine end
    local e = FindFirstOf("GameEngine")
    if not Valid(e) then e = FindFirstOf("Engine") end
    engine = e
    return Valid(e) and e or nil
end

-- The controller that owns the local player right now (the debug camera's controller while that camera is on).
function M.LocalPC()
    local ok, pc = pcall(function()
        local e = Engine(); if not e then return nil end
        local gi = e.GameInstance
        if not Valid(gi) then return nil end
        local lps = gi.LocalPlayers
        local lp = lps[1]
        if not Valid(lp) then return nil end
        return lp.PlayerController
    end)
    if ok and Valid(pc) then return pc end
    -- engine path not available in this game: search, but at most every 2 seconds, and keep the answer
    if Valid(fallbackPC) then return fallbackPC end
    if os.clock() - fallbackAt < 2 then return nil end
    fallbackAt = os.clock()
    local s = slowGetPC()
    fallbackPC = Valid(s) and s or nil
    return fallbackPC
end

function M.IsDebugPC(pc)
    if not Valid(pc) then return false end
    local ok, n = pcall(function() return pc:GetClass():GetFName():ToString() end)
    return ok and type(n) == "string" and n:find("DebugCamera") ~= nil
end

-- The real player's controller (not the debug camera's).
function M.PlayerPC()
    local pc = M.LocalPC()
    if M.IsDebugPC(pc) then
        local ok, orig = pcall(function() return pc.OriginalControllerRef end)
        if ok and Valid(orig) then return orig end
        return nil
    end
    return pc
end

-- How long UE4SS's own search takes in this game (for the perf test)
function M.SlowSearch() return slowGetPC() end

-- Make UE4SS's helpers fast for this mod too (GetWorld uses GetPlayerController)
UEHelpers.GetPlayerController = function()
    local pc = M.LocalPC()
    if pc then return pc end
    return CreateInvalidObject()
end

return M
