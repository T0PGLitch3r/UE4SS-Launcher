-- LauncherBridge: lets the UE4SS Launcher list this game's levels and switch between them.
-- It talks to the launcher through small text files in %TEMP%\UE4SS-LauncherBridge.
--   command.txt  (launcher -> game)  "<id>|<verb>|<arg>"   verbs: list, open, reload
--   result.txt   (game -> launcher)  "<id>|ok|<message>" or "<id>|error|<message>"
--   levels.txt   (game -> launcher)  one level package path per line
--   status.txt   (game -> launcher)  heartbeat with the current level

local UEHelpers = require("UEHelpers")
local Freecam = require("freecam")
local Editor = require("editor")

local function Log(msg) print("[LauncherBridge] " .. tostring(msg) .. "\n") end

-- ---------------------------------------------------------------- bridge folder
local function FindBridgeDir()
    local temp = os.getenv("TEMP") or os.getenv("TMP")
    if temp then
        local dir = temp .. "\\UE4SS-LauncherBridge\\"
        local probe = io.open(dir .. "probe.tmp", "w")   -- the launcher creates this folder
        if probe then probe:close(); os.remove(dir .. "probe.tmp"); return dir end
    end
    -- Fallback: a "bridge" folder next to this mod (shipped with it).
    for p in string.gmatch(package.path or "", "[^;]+") do
        local base = p:match("^(.-[/\\]LauncherBridge)[/\\]Scripts[/\\]")
        if base then return base .. "\\bridge\\" end
    end
    local src = debug.getinfo(1, "S").source:gsub("^@", "")
    local base = src:match("^(.-[/\\]LauncherBridge)[/\\]Scripts[/\\]")
    return (base or ".") .. "\\bridge\\"
end
local BridgeDir = FindBridgeDir()
Log("Using bridge folder " .. BridgeDir)

local function ReadFile(path)
    local f = io.open(path, "r")
    if not f then return nil end
    local d = f:read("a")
    f:close()
    return d
end
local function WriteFile(path, data)
    local tmp = path .. ".tmp"
    local f = io.open(tmp, "w")
    if not f then return false end
    f:write(data)
    f:close()
    os.remove(path)
    return os.rename(tmp, path)
end

-- ---------------------------------------------------------------- helpers
local function Str(v)
    if v == nil then return nil end
    if type(v) == "string" then return v end
    local ok, s = pcall(function() return v:ToString() end)
    if ok and s then return s end
    return tostring(v)
end
local function Field(obj, key)
    local ok, v = pcall(function() return obj[key] end)
    if ok then return v end
    return nil
end
-- Out-param array elements may come back as the struct itself or wrapped (needing :get()).
local function Unwrap(e)
    local direct = Field(e, "PackageName") or Field(e, "ObjectPath")
    if direct ~= nil then return e end
    local ok, g = pcall(function() return e:get() end)
    if ok and g ~= nil then return g end
    return e
end
local function FN(s) return UEHelpers.FindOrAddFName(s) end

-- "World /Game/Maps/Level01.Level01" -> "/Game/Maps/Level01"
local function PackageFromFullName(full)
    if not full then return nil end
    local path = full:match("^%S+%s+(%S+)") or full
    return path:match("^([^%.:]+)") or path
end

local CurrentMap = "unknown"
local function UpdateCurrentMap(world)
    local ok, full = pcall(function() return world:GetFullName() end)
    if ok and full then CurrentMap = PackageFromFullName(full) or CurrentMap end
end

-- ---------------------------------------------------------------- finding levels
local function GetAssetRegistry()
    local candidates = {}
    local helpers = StaticFindObject("/Script/AssetRegistry.Default__AssetRegistryHelpers")
    if helpers and helpers:IsValid() then
        local ok, reg = pcall(function() return helpers:GetAssetRegistry() end)
        if ok and reg and reg.IsValid and reg:IsValid() then table.insert(candidates, reg) end
    end
    local impl = StaticFindObject("/Script/AssetRegistry.Default__AssetRegistryImpl")
    if impl and impl:IsValid() then table.insert(candidates, impl) end
    return candidates
end

local function CollectWorlds(out, into, seen, needClassCheck)
    for _, e in ipairs(out) do
        local a = Unwrap(e)
        local pkg = Str(Field(a, "PackageName"))
        if not pkg or pkg == "" or pkg == "None" then
            pkg = PackageFromFullName(Str(Field(a, "ObjectPath")))
        end
        local isWorld = true
        if needClassCheck then
            local cls = Str(Field(a, "AssetClass"))
            if not cls or cls == "None" or cls == "" then
                local cp = Field(a, "AssetClassPath")
                cls = cp and Str(Field(cp, "AssetName")) or nil
            end
            isWorld = (cls == "World")
        end
        if isWorld and pkg and pkg ~= "" and pkg ~= "None" and not seen[pkg] then
            seen[pkg] = true
            table.insert(into, pkg)
        end
    end
end

-- The asset-registry query is only safe when we call it with exactly the parameter types the engine
-- expects. UE4SS doesn't type-check FName/struct parameters, so a mismatch can crash the game
-- (this happened on Stray / UE 4.27). We therefore only use the UE 5.1+ signature, and on older
-- engines fall back to the worlds already loaded (the launcher reads the rest from the .pak files).
local function RegistryIsSafe()
    local ok, below = pcall(function() return UnrealVersion.IsBelow(5, 1) end)
    return ok and below == false
end

local function FindLevels()
    local levels, seen, method = {}, {}, "none"
    local registries = RegistryIsSafe() and GetAssetRegistry() or {}
    for _, reg in ipairs(registries) do
        -- UE 5.1+: GetAssetsByClass(FTopLevelAssetPath, out, bSearchSubClasses)
        local out = {}
        local ok = pcall(function() reg:GetAssetsByClass({ PackageName = FN("/Script/Engine"), AssetName = FN("World") }, out, false) end)
        if ok and #out > 0 then CollectWorlds(out, levels, seen, false); method = "AssetRegistry (UE5)" end
        if #levels > 0 then break end
    end
    -- Always include worlds that are loaded in memory right now.
    local worlds = FindAllOf("World")
    if worlds then
        for _, w in ipairs(worlds) do
            local pkg = PackageFromFullName(Str(w:GetFullName()))
            if pkg and pkg:sub(1, 1) == "/" and not seen[pkg] and not pkg:find("/Temp/") then
                seen[pkg] = true
                table.insert(levels, pkg)
                if method == "none" then method = "loaded worlds only" end
            end
        end
    end
    table.sort(levels)
    return levels, method
end

-- ---------------------------------------------------------------- commands
local function Reply(id, ok, msg)
    WriteFile(BridgeDir .. "result.txt", string.format("%s|%s|%s", id, ok and "ok" or "error", msg or ""))
end

local function OpenLevel(id, path)
    ExecuteInGameThread(function()
        local world = UEHelpers.GetWorld()
        if not world or not world:IsValid() then Reply(id, false, "No world is loaded yet."); return end
        Log("Opening level " .. path)
        local ok, err = pcall(function()
            UEHelpers.GetGameplayStatics():OpenLevel(world, FN(path), true, "")
        end)
        if not ok then
            Log("OpenLevel failed (" .. tostring(err) .. "), trying the 'open' console command")
            ok, err = pcall(function()
                UEHelpers.GetKismetSystemLibrary():ExecuteConsoleCommand(world, "open " .. path, UEHelpers.GetPlayerController())
            end)
        end
        if ok then Reply(id, true, "Opening " .. path) else Reply(id, false, "Couldn't open " .. path .. ": " .. tostring(err)) end
    end)
end

local function ListLevels(id)
    ExecuteInGameThread(function()
        local ok, levels, method = pcall(FindLevels)
        if not ok then Reply(id, false, "Level scan failed: " .. tostring(levels)); return end
        local data = "#method " .. method .. "\n" .. table.concat(levels, "\n") .. "\n"
        WriteFile(BridgeDir .. "levels.txt", data)
        Log(string.format("Found %d levels via %s", #levels, method))
        Reply(id, true, string.format("Found %d levels", #levels))
    end)
end

-- perf test: frame timing for N seconds with the old player lookup (a full object search once a second),
-- then N seconds with the new one. Written to perf.txt in the bridge folder.
local Fast = require("fast")
local perf = nil
function StartPerf(id, secs)
    perf = { id = id, secs = math.max(3, math.min(60, secs)), phase = 1, t0 = os.clock(), last = os.clock(), lastSlow = 0, d = { {}, {} }, slowMs = {} }
    Log("perf test started")
end
local function PerfTick()
    if not perf then return end
    local now = os.clock()
    table.insert(perf.d[perf.phase], (now - perf.last) * 1000); perf.last = now
    if perf.phase == 1 and now - perf.lastSlow >= 1 then
        perf.lastSlow = now
        local a = os.clock(); local _ = Fast.SlowSearch(); table.insert(perf.slowMs, (os.clock() - a) * 1000)
        perf.last = os.clock()
    elseif perf.phase == 2 and now - perf.lastSlow >= 1 then
        perf.lastSlow = now
        local a = os.clock(); local _ = Fast.LocalPC(); perf.fastMs = (os.clock() - a) * 1000
    end
    if now - perf.t0 >= perf.secs then
        if perf.phase == 1 then perf.phase = 2; perf.t0 = now; return end
        local function stats(t) local n, sum, mx, big = #t, 0, 0, 0; for _, v in ipairs(t) do sum = sum + v; if v > mx then mx = v end; if v > 50 then big = big + 1 end end
            return string.format("ticks=%d avg=%.1fms max=%.1fms over50ms=%d", n, n > 0 and sum / n or 0, mx, big) end
        local sm = 0; for _, v in ipairs(perf.slowMs) do sm = sm + v end
        local out = string.format("old: %s | full search avg %.1fms\nnew: %s | fast lookup %.3fms\nsaved changes: slowest put-back %.1fms, slowest single lookup %.1fms\n", stats(perf.d[1]), #perf.slowMs > 0 and sm / #perf.slowMs or 0, stats(perf.d[2]), perf.fastMs or -1, Editor.layMs or 0, Editor.lookMs or 0)
        Editor.layMs = 0; Editor.lookMs = 0
        WriteFile(BridgeDir .. "perf.txt", out)
        Reply(perf.id, true, out:gsub("\n", " / "))
        perf = nil
    end
end

local function HandleCommand(line)
    local id, verb, arg = line:match("^(%d+)|([%w_]+)|?(.*)$")
    if not id then return end
    arg = (arg or ""):gsub("[\r\n]", "")
    if Editor.Handle(verb, arg) then return end
    if verb == "perf" then StartPerf(id, tonumber(arg) or 10)
    elseif verb == "list" then ListLevels(id)
    elseif verb == "open" and arg ~= "" then OpenLevel(id, arg)
    elseif verb == "reload" then
        if CurrentMap ~= "unknown" then OpenLevel(id, CurrentMap) else Reply(id, false, "Current level isn't known yet.") end
    elseif verb == "freecam" then
        ExecuteInGameThread(function()
            local want = nil
            if arg == "on" then want = true elseif arg == "off" then want = false end
            local ok, res, msg = pcall(Freecam.Toggle, want)
            if ok then Reply(id, res, msg) else Reply(id, false, "Freecam error: " .. tostring(res)) end
        end)
    elseif verb == "fcspeed" then
        ExecuteInGameThread(function()
            local v = Freecam.SetSpeed(tonumber(arg) or 1.0)
            Reply(id, true, string.format("Freecam speed %.2fx", v))
        end)
    elseif verb == "tp" then
        ExecuteInGameThread(function()
            local ok, res, msg = pcall(Freecam.TeleportPlayer)
            if ok then Reply(id, res, msg) else Reply(id, false, "Teleport error: " .. tostring(res)) end
        end)
    elseif verb == "ping" then Reply(id, true, "pong")
    else Reply(id, false, "Unknown command: " .. verb) end
end

-- ---------------------------------------------------------------- hooks & loop
-- New level loaded: noticed by the bridge loop (the world object changes). UE4SS's own "map loaded" hook isn't
-- used any more: on some games (High on Life) it hands Lua a broken level address and the game crashes.
local lastWorldName = nil
local function CheckWorldChanged()
    local w = UEHelpers.GetWorld()
    if not w or not w:IsValid() then return end
    local ok, full = pcall(function() return w:GetFullName() end)
    if not ok or not full then return end
    local addr = ""
    pcall(function() addr = string.format("%x", w:GetAddress()) end)
    local key = full .. "@" .. addr
    if key == lastWorldName then return end
    local first = lastWorldName == nil
    lastWorldName = key
    UpdateCurrentMap(w)
    if not first then
        pcall(Freecam.OnMapChanged)
        pcall(Editor.OnMapChanged)
    end
end
Editor.Init(BridgeDir, Freecam)

-- Freecam settings come from the launcher (config.txt) and are re-read every couple of seconds.
Freecam.dir = BridgeDir
Freecam.ApplyConfig(ReadFile(BridgeDir .. "config.txt"))
Freecam.RegisterHotkeys()
Freecam.RegisterSpeedKeys()
ExecuteInGameThread(function()
    local w = UEHelpers.GetWorld()
    if w and w:IsValid() then UpdateCurrentMap(w) end
end)

local beat = 0
local modMs = 0      -- time this mod spent on the game thread since the last heartbeat (reported in status.txt)
local frameQueued = false
-- Move the freecam inside Unreal's own per-frame camera update, right before the view is worked out:
-- every frame, at the game's frame rate, with no delay. (Falls back to the timer job above if a game
-- never calls it.)
local camHookAt = -1
pcall(function()
    RegisterHook("/Script/Engine.PlayerCameraManager:BlueprintUpdateCamera", function()
        if Editor.PossessActive() then Editor.possByHook = true; pcall(Editor.PossessFrame) end
        if Editor.ViewMode and Editor.ViewMode() ~= "off" then Editor.viewByHook = true; pcall(Editor.ViewFrame) end
        if not Freecam.active then return end
        camHookAt = os.clock()
        local t0 = camHookAt
        pcall(Freecam.Frame)
        modMs = modMs + (os.clock() - t0) * 1000
    end)
    Log("Camera hook ready")
end)
-- The bridge loop. It runs on the GAME thread (LoopInGameThreadWithDelay) so it never runs at the same moment as
-- the camera hook: Lua isn't thread-safe, and the old LoopAsync timer (a second thread) slowly corrupted the
-- mod's Lua state until the game crashed. LoopAsync is only used on old UE4SS builds that can't do this.
local function Tick(onGameThread)
    if onGameThread then pcall(PerfTick) end
    -- Nothing going on (editor closed, freecam off): look for commands about 15 times a second instead of every
    -- frame. Opening a file every frame on the game thread shows up as lag in big games (and antivirus scans it).
    local idle = not (Freecam.active or Editor.Busy())
    beat = beat + 1
    if idle and beat % 4 ~= 0 then return end
    beat = beat - 1
    -- The launcher writes one or more commands (one per line) and only writes again once we've removed the file.
    local cmd = ReadFile(BridgeDir .. "command.txt")
    if cmd and cmd ~= "" then
        os.remove(BridgeDir .. "command.txt")
        for line in cmd:gmatch("[^\r\n]+") do
            local ok, err = pcall(HandleCommand, line)
            if not ok then Log("Command failed: " .. tostring(err)) end
        end
    end
    -- per-frame work (editing camera, editor state, gizmo camera, model pictures)
    if Freecam.active or Editor.Busy() then
        local camByHook = (os.clock() - camHookAt) < 0.25   -- the camera hook is moving the freecam every frame
        if onGameThread then
            if not camByHook then pcall(Freecam.Frame) end
            pcall(Editor.Frame)
        elseif not frameQueued then
            frameQueued = true
            ExecuteInGameThread(function()
                if not camByHook then pcall(Freecam.Frame) end
                pcall(Editor.Frame)
                frameQueued = false
            end)
        end
    end
    beat = beat + 1
    if beat % 20 == 0 then
        if onGameThread then pcall(CheckWorldChanged) else ExecuteInGameThread(function() pcall(CheckWorldChanged) end) end
    end
    if beat % 60 == 0 then  -- heartbeat about once a second
        local spent = modMs; modMs = 0
        WriteFile(BridgeDir .. "status.txt", string.format("map=%s\nfreecam=%s\nfcmode=%s\ncam=%s\nrot=%s\nfckey=%s\nfcspeed=%.3f\nmodms=%.1f\n",
            tostring(CurrentMap), Freecam.active and "on" or "off", Freecam.kind, Freecam.camText, Freecam.rotText, Freecam.cfg.key, Freecam.speed, spent))
        if onGameThread then pcall(Freecam.UpdateCameraInfo) else ExecuteInGameThread(function() pcall(Freecam.UpdateCameraInfo) end) end
    end
    if beat % 120 == 0 then Freecam.ApplyConfig(ReadFile(BridgeDir .. "config.txt")) end
end

local loopStarted = false
if type(LoopInGameThreadWithDelay) == "function" and EngineTickAvailable ~= false then
    loopStarted = pcall(LoopInGameThreadWithDelay, 16, function()
        local t0 = os.clock()
        local ok, err = pcall(Tick, true)
        modMs = modMs + (os.clock() - t0) * 1000
        if not ok then Log("Loop error: " .. tostring(err)) end
    end)
    if loopStarted then Log("Bridge loop on the game thread") end
end
if not loopStarted then
    LoopAsync(16, function()
        local ok, err = pcall(Tick, false)
        if not ok then Log("Loop error: " .. tostring(err)) end
        return false  -- keep looping
    end)
    Log("Bridge loop on a timer thread (this UE4SS can't loop on the game thread)")
end

Log("Ready")
