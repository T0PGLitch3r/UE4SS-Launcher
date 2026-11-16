-- Freecam for any Unreal Engine game.
-- 1) Unreal's own debug camera (CheatManager:ToggleDebugCamera) - full mouse/WASD flying, built into most games.
-- 2) If a game has that compiled out, a simple built-in freecam: spawns a CameraActor, views through it and
--    flies it with WASD / mouse / arrow keys. Every engine call uses exactly the parameter types it expects.
local UEHelpers = require("UEHelpers")

local F = {}
F.cfg = { enabled = true, key = "INS", mode = "auto", hideText = true, speed = 1.0 }
F.active = false
F.kind = "off"            -- "debug" | "builtin" | "off"
F.camText = ""            -- "x,y,z" of the current view
F.rotText = ""            -- "pitch,yaw"
F.speed = 1.0             -- current speed multiplier (shared by both camera types)
F.ext = false             -- flown by the launcher (in-game editor: controller / right mouse + WASD) instead of keys
F.dir = nil               -- bridge folder (set by main.lua), where the launcher writes fly.txt
local MIN_SPEED, MAX_SPEED = 0.05, 50
local lastCfgSpeed = nil  -- last speed read from the launcher's config

local state = {}
local debugUnavailable = false
local mouseOK = true

local function Log(m) print("[LauncherBridge] [Freecam] " .. tostring(m) .. "\n") end
local function Valid(o)
    if o == nil then return false end
    local ok, v = pcall(function() return o:IsValid() end)
    return ok and v == true
end
local function FN(s) return UEHelpers.FindOrAddFName(s) end
local function Vec(x, y, z) return { X = x, Y = y, Z = z } end
local function Rot(p, y, r) return { Pitch = p, Yaw = y, Roll = r } end

-- The controller that currently owns the local player (switches to the DebugCameraController while it's active).
local Fast = require("fast")
function F.ActivePC()
    return Fast.LocalPC()      -- straight from the engine (searching every object made big games stutter)
end

local function IsDebugController(pc)
    if not Valid(pc) then return false end
    local ok, n = pcall(function() return pc:GetClass():GetFName():ToString() end)
    return ok and type(n) == "string" and n:find("DebugCamera") ~= nil
end

local function EnsureCheatManager(pc)
    local cm = pc.CheatManager
    if Valid(cm) then return cm end
    local cls = pc.CheatClass
    if not Valid(cls) then cls = StaticFindObject("/Script/Engine.CheatManager") end
    if not Valid(cls) then return nil end
    cm = StaticConstructObject(cls, pc)
    if Valid(cm) then pc.CheatManager = cm; return cm end
    return nil
end

-- ---------------------------------------------------------------- speed
local baseMove = nil  -- spectator pawn's original movement values (for games without SetPawnMovementSpeedScale)

local function ApplyDebugSpeed(dcc, scale)
    if not Valid(dcc) then return false end
    -- UE 4.25+: one call does it (also used by the mouse wheel)
    if pcall(function() dcc:SetPawnMovementSpeedScale(scale) end) then return true end
    -- older engines: scale the spectator pawn's movement component directly
    local ok = pcall(function()
        dcc.SpeedScale = scale
        local pawn = dcc.Pawn
        if not Valid(pawn) then pawn = dcc:GetSpectatorPawn() end
        local mc = pawn.MovementComponent
        if not Valid(mc) then return end
        if not baseMove then baseMove = { mc.MaxSpeed, mc.Acceleration, mc.Deceleration } end
        mc.MaxSpeed, mc.Acceleration, mc.Deceleration = baseMove[1] * scale, baseMove[2] * scale, baseMove[3] * scale
    end)
    return ok
end

local function DebugController()
    local pc = F.ActivePC()
    if Valid(pc) then
        local ok, n = pcall(function() return pc:GetClass():GetFName():ToString() end)
        if ok and type(n) == "string" and n:find("DebugCamera") then return pc end
    end
    return nil
end

-- Sets the speed for whichever freecam is running (game thread).
function F.SetSpeed(v)
    F.speed = math.max(MIN_SPEED, math.min(MAX_SPEED, v))
    if F.active and F.kind == "debug" then ApplyDebugSpeed(DebugController(), F.speed) end
    return F.speed
end

function F.ChangeSpeed(factor)
    if not F.active then return end
    local v = F.SetSpeed(F.speed * factor)
    Log(string.format("Speed %.2fx", v))
end

-- ---------------------------------------------------------------- 1) Unreal debug camera
local function DebugCamOn()
    local pc = F.ActivePC()
    if not Valid(pc) then return false, "No player yet - load into the game first." end
    local cm = EnsureCheatManager(pc)
    if not cm then return false, "Couldn't create a CheatManager." end
    local ok, err = pcall(function() cm:ToggleDebugCamera() end)
    if not ok then return false, tostring(err) end
    local now = F.ActivePC()
    if IsDebugController(now) then
        if F.cfg.hideText then pcall(function() now:ToggleDisplay() end) end
        baseMove = nil
        ApplyDebugSpeed(now, F.speed)
        return true
    end
    return false, "unavailable"
end

local function DebugCamOff()
    local pc = F.ActivePC()
    if not IsDebugController(pc) then return true end
    local cm = EnsureCheatManager(pc)
    if cm then pcall(function() cm:ToggleDebugCamera() end) end
    if IsDebugController(F.ActivePC()) then
        pcall(function() UEHelpers.GetKismetSystemLibrary():ExecuteConsoleCommand(pc:GetWorld(), "ToggleDebugCamera", pc) end)
    end
    return not IsDebugController(F.ActivePC())
end

-- ---------------------------------------------------------------- 2) built-in simple freecam
local keys = {}
local function K(name)
    if not keys[name] then keys[name] = { KeyName = FN(name) } end
    return keys[name]
end

local function BuiltinOn()
    local pc = F.ActivePC()
    if not Valid(pc) then return false, "No player yet - load into the game first." end
    local pcm = pc.PlayerCameraManager
    if not Valid(pcm) then return false, "The game has no camera manager right now." end
    local loc, rot = pcm:GetCameraLocation(), pcm:GetCameraRotation()
    state.x, state.y, state.z = loc.X, loc.Y, loc.Z
    state.pitch, state.yaw = rot.Pitch, rot.Yaw
    local cls = StaticFindObject("/Script/Engine.CameraActor")
    if not Valid(cls) then return false, "CameraActor class not found." end
    local world = pc:GetWorld()
    local loc, rot = Vec(state.x, state.y, state.z), Rot(state.pitch, state.yaw, 0)
    local okS, cam = pcall(function() return world:SpawnActor(cls, loc, rot) end)
    if not (okS and Valid(cam)) then
        -- the engine's own spawn (works where UE4SS's SpawnActor isn't available)
        local gs = StaticFindObject("/Script/Engine.Default__GameplayStatics")
        local tf = { Translation = loc, Rotation = { X = 0, Y = 0, Z = 0, W = 1 }, Scale3D = Vec(1, 1, 1) }
        okS, cam = pcall(function() return gs:BeginDeferredActorSpawnFromClass(world, cls, tf, 1, nil) end)
        if okS and Valid(cam) then pcall(function() gs:FinishSpawningActor(cam, tf) end) end
    end
    if not Valid(cam) then return false, "Couldn't spawn a camera." end
    pcall(function() cam:K2_TeleportTo(loc, rot) end)
    state.cam, state.pc = cam, pc
    local okT, tgt = pcall(function() return pc:GetViewTarget() end)
    state.oldTarget = okT and tgt or nil
    pcall(function() pc:SetIgnoreMoveInput(true) end)
    pcall(function() pc:SetIgnoreLookInput(true) end)
    pc:SetViewTargetWithBlend(cam, 0.0, 0, 0.0, false)
    return true
end

local function BuiltinOff()
    local pc = state.pc
    if Valid(pc) then
        pcall(function() pc:SetIgnoreMoveInput(false) end)
        pcall(function() pc:SetIgnoreLookInput(false) end)
        local target = Valid(state.oldTarget) and state.oldTarget or pc.Pawn
        if Valid(target) then pcall(function() pc:SetViewTargetWithBlend(target, 0.0, 0, 0.0, false) end) end
    end
    if Valid(state.cam) then pcall(function() state.cam:K2_DestroyActor() end) end
    state = {}
    return true
end

-- Launcher-driven flying: fly.txt = "seq yawTotal pitchTotal forward right up speed"
-- (look totals only ever grow, so a missed update loses nothing; movement is a -1..1 stick value per axis)
local ext = { seq = -1, yaw = nil, pitch = nil, f = 0, r = 0, u = 0, speed = 1, at = 0 }
local function ReadExt()
    if not F.dir then return end
    local fh = io.open(F.dir .. "fly.txt", "r"); if not fh then return end
    local line = fh:read("*l"); fh:close()
    if not line then return end
    local seq, yw, pt, f, r, u, sp = line:match("^(%d+) (%S+) (%S+) (%S+) (%S+) (%S+) (%S+)")
    seq = tonumber(seq); if not seq then return end
    yw, pt = tonumber(yw), tonumber(pt)
    if seq ~= ext.seq then
        if ext.yaw and yw and pt then              -- new look input goes to where the camera is heading
            state.tyaw = (state.tyaw or state.yaw) + (yw - ext.yaw)
            state.tpitch = math.max(-89, math.min(89, (state.tpitch or state.pitch) + (pt - ext.pitch)))
        end
        ext.seq, ext.yaw, ext.pitch = seq, yw, pt
        ext.f, ext.r, ext.u, ext.speed = tonumber(f) or 0, tonumber(r) or 0, tonumber(u) or 0, tonumber(sp) or 1
        ext.at = os.clock()
    end
end

-- One smooth step of the launcher-flown camera: turning eases towards where you're looking (no stepping),
-- and movement speeds up / slows down over a fraction of a second instead of starting and stopping dead.
local LOOK_SMOOTH, MOVE_SMOOTH = 0.06, 0.14        -- seconds
local function ExtStep(dt)
    ReadExt()
    local now = os.clock()
    local stale = (now - ext.at) > 0.3             -- launcher stopped sending: glide to a stop
    local f, r, u = stale and 0 or ext.f, stale and 0 or ext.r, stale and 0 or ext.u
    state.tyaw = state.tyaw or state.yaw
    state.tpitch = state.tpitch or state.pitch
    local kl = 1 - math.exp(-dt / LOOK_SMOOTH)
    state.yaw = state.yaw + (state.tyaw - state.yaw) * kl
    state.pitch = math.max(-89, math.min(89, state.pitch + (state.tpitch - state.pitch) * kl))
    local p, y = math.rad(state.pitch), math.rad(state.yaw)
    local fx, fy, fz = math.cos(p) * math.cos(y), math.cos(p) * math.sin(y), math.sin(p)
    local rx, ry = -math.sin(y), math.cos(y)
    local sp = 1200 * ext.speed
    local wx, wy, wz = (fx * f + rx * r) * sp, (fy * f + ry * r) * sp, (fz * f + u) * sp
    state.vx, state.vy, state.vz = state.vx or 0, state.vy or 0, state.vz or 0
    local km = 1 - math.exp(-dt / MOVE_SMOOTH)
    state.vx = state.vx + (wx - state.vx) * km
    state.vy = state.vy + (wy - state.vy) * km
    state.vz = state.vz + (wz - state.vz) * km
    state.x, state.y, state.z = state.x + state.vx * dt, state.y + state.vy * dt, state.z + state.vz * dt
end

local updating = false
local lastClock = nil
local function BuiltinUpdate(dt)
    local pc, cam = state.pc, state.cam
    if not Valid(pc) or not Valid(cam) then F.active = false; F.kind = "off"; state = {}; return end
    local now = os.clock()
    if lastClock then
        if now - lastClock < 0.0005 then return end   -- already stepped this frame
        dt = math.min(0.1, now - lastClock)
    end
    lastClock = now
    if F.ext then
        ExtStep(dt)
        pcall(function() cam:K2_TeleportTo(Vec(state.x, state.y, state.z), Rot(state.pitch, state.yaw, 0)) end)
        return
    end
    local function down(k)
        local ok, v = pcall(function() return pc:IsInputKeyDown(K(k)) end)
        return ok and v == true
    end
    -- look: mouse (if the game reports it) and arrow keys
    if mouseOK then
        local t = {}
        local ok = pcall(function() pc:GetInputMouseDelta(t, t) end)
        if ok and type(t.DeltaX) == "number" then
            state.yaw = state.yaw + t.DeltaX * 2.5
            state.pitch = state.pitch + t.DeltaY * 2.5
        else mouseOK = false end
    end
    local turn = 90 * dt
    if down("Left") then state.yaw = state.yaw - turn end
    if down("Right") then state.yaw = state.yaw + turn end
    if down("Up") then state.pitch = state.pitch + turn end
    if down("Down") then state.pitch = state.pitch - turn end
    state.pitch = math.max(-89, math.min(89, state.pitch))
    -- move
    local speed = 1200 * F.speed * dt
    if down("LeftShift") then speed = speed * 4 end
    if down("LeftControl") then speed = speed * 0.25 end
    local p, y = math.rad(state.pitch), math.rad(state.yaw)
    local fx, fy, fz = math.cos(p) * math.cos(y), math.cos(p) * math.sin(y), math.sin(p)
    local rx, ry = -math.sin(y), math.cos(y)
    local mx, my, mz = 0, 0, 0
    if down("W") then mx, my, mz = mx + fx, my + fy, mz + fz end
    if down("S") then mx, my, mz = mx - fx, my - fy, mz - fz end
    if down("D") then mx, my = mx + rx, my + ry end
    if down("A") then mx, my = mx - rx, my - ry end
    if down("E") or down("SpaceBar") then mz = mz + 1 end
    if down("Q") then mz = mz - 1 end
    state.x, state.y, state.z = state.x + mx * speed, state.y + my * speed, state.z + mz * speed
    pcall(function() cam:K2_TeleportTo(Vec(state.x, state.y, state.z), Rot(state.pitch, state.yaw, 0)) end)
end

-- Launcher-driven flying of Unreal's debug camera: we move its spectator pawn and set its look direction
-- ourselves (its own keyboard / mouse / controller input is held back by the editor while it's open).
local dbgReady = false
local dbgLast = nil
local dbgPcm = nil
local diag = { at = 0, frames = 0, moved = 0, turned = 0, maxd = 0, maxa = 0 }
local CalmDebugCam, RestoreCalm
local calmed = {}            -- what CalmDebugCam changed, put back by RestoreCalm
local function Kismet() return UEHelpers.GetKismetSystemLibrary() end
-- Makes the editing camera steady: no game camera shakes / effects on it, its own input held back, and the
-- game's motion blur off while flying it (like Unreal's editor viewport). Everything is put back afterwards.
CalmDebugCam = function(dcc, pawn)
    RestoreCalm()
    dbgPcm = nil
    pcall(function() dbgPcm = dcc.PlayerCameraManager end)
    local info = {}
    pcall(function() info[#info + 1] = "controller " .. dcc:GetClass():GetFName():ToString() end)
    pcall(function() info[#info + 1] = "pawn " .. pawn:GetClass():GetFName():ToString() end)
    if Valid(dbgPcm) then
        pcall(function() info[#info + 1] = "camera manager " .. dbgPcm:GetClass():GetFName():ToString() end)
        -- camera modifiers only when the debug camera has its own camera manager (never touch the game's)
        local gamePcm = nil
        pcall(function() local o = dcc.OriginalControllerRef; if Valid(o) then gamePcm = o.PlayerCameraManager end end)
        local shared = Valid(gamePcm) and gamePcm:GetFullName() == dbgPcm:GetFullName()
        if not shared then
            local n = 0
            pcall(function()
                dbgPcm.ModifierList:ForEach(function(_, m)
                    local mod = m:get()
                    if Valid(mod) then pcall(function() mod:DisableModifier(true) end); n = n + 1 end
                end)
            end)
            if n > 0 then info[#info + 1] = n .. " camera modifiers off" end
        end
        pcall(function() dbgPcm:StopAllCameraShakes(true) end)
    end
    pcall(function() if not dcc:IsLookInputIgnored() then dcc:SetIgnoreLookInput(true) end end)
    pcall(function() if not dcc:IsMoveInputIgnored() then dcc:SetIgnoreMoveInput(true) end end)
    -- the game's motion blur and depth of field (South of Midnight blurs everything not near the player) off
    -- while flying, like Unreal's editor viewport; put back to what they were afterwards
    calmed.cvars = {}
    local pcArg = nil
    pcall(function() pcArg = dcc end)
    for _, cv in ipairs({ { "r.MotionBlurQuality", 4 }, { "r.DepthOfFieldQuality", 2 } }) do
        local was = nil
        pcall(function() was = Kismet():GetConsoleVariableIntValue(cv[1]) end)
        if type(was) ~= "number" then was = cv[2] end
        if was ~= 0 then
            local ok = pcall(function() Kismet():ExecuteConsoleCommand(dcc:GetWorld(), cv[1] .. " 0", pcArg) end)
            local now = "?"
            pcall(function() now = tostring(Kismet():GetConsoleVariableIntValue(cv[1])) end)
            if ok then calmed.cvars[#calmed.cvars + 1] = { cv[1], was }; info[#info + 1] = cv[1] .. " " .. was .. " -> " .. now end
        end
    end
    -- and in case the game won't take those commands: a post-process volume over the whole level that turns
    -- motion blur and depth of field off (removed again when the editor closes)
    pcall(function()
        local cls = StaticFindObject("/Script/Engine.PostProcessVolume")
        if not Valid(cls) then return end
        local w = dcc:GetWorld()
        local l = pawn:K2_GetActorLocation()
        -- set up before it's finished spawning: the level keeps its volumes sorted by priority when they're added
        local function Setup(vol)
            vol.bUnbound = true
            vol.bEnabled = true
            vol.Priority = 1000000
            vol.BlendWeight = 1.0
            local st = vol.Settings
            st.bOverride_MotionBlurAmount = true;          st.MotionBlurAmount = 0.0
            st.bOverride_MotionBlurMax = true;             st.MotionBlurMax = 0.0
            st.bOverride_DepthOfFieldFocalDistance = true; st.DepthOfFieldFocalDistance = 0.0
            st.bOverride_DepthOfFieldFstop = true;         st.DepthOfFieldFstop = 32.0
            st.bOverride_DepthOfFieldMinFstop = true;      st.DepthOfFieldMinFstop = 32.0
            st.bOverride_DepthOfFieldScale = true;         st.DepthOfFieldScale = 0.0
            st.bOverride_DepthOfFieldNearBlurSize = true;  st.DepthOfFieldNearBlurSize = 0.0
            st.bOverride_DepthOfFieldFarBlurSize = true;   st.DepthOfFieldFarBlurSize = 0.0
        end
        local vol = nil
        local gs = StaticFindObject("/Script/Engine.Default__GameplayStatics")
        local tf = { Translation = l, Rotation = { X = 0, Y = 0, Z = 0, W = 1 }, Scale3D = Vec(1, 1, 1) }
        pcall(function() vol = gs:BeginDeferredActorSpawnFromClass(w, cls, tf, 1, nil) end)
        local okS, eS = true, nil
        if Valid(vol) then
            okS, eS = pcall(Setup, vol)
            pcall(function() gs:FinishSpawningActor(vol, tf) end)
        else
            pcall(function() vol = w:SpawnActor(cls, l, Rot(0, 0, 0)) end)
            if Valid(vol) then okS, eS = pcall(Setup, vol) end
        end
        if not okS then info[#info + 1] = "blur volume setup: " .. tostring(eS) end
        if not Valid(vol) then info[#info + 1] = "no blur volume"; return end
        calmed.vol = vol
        info[#info + 1] = "blur volume on"
    end)
    pcall(function()
        calmed.cursor = dcc.bShowMouseCursor
        info[#info + 1] = "pointer " .. (calmed.cursor and "shown -> hidden" or "hidden")
        dcc.bShowMouseCursor = false
    end)
    calmed.dcc = dcc
    Log("editing camera: " .. table.concat(info, ", "))
end
RestoreCalm = function()
    if Valid(calmed.dcc) then
        pcall(function() calmed.dcc:ResetIgnoreLookInput() end)
        pcall(function() calmed.dcc:ResetIgnoreMoveInput() end)
        if calmed.cursor then pcall(function() calmed.dcc.bShowMouseCursor = true end) end
    end
    if Valid(calmed.vol) then pcall(function() calmed.vol:K2_DestroyActor() end) end
    for _, cv in ipairs(calmed.cvars or {}) do
        pcall(function()
            local pc = F.ActivePC()
            Kismet():ExecuteConsoleCommand(pc and pc:GetWorld() or nil, cv[1] .. " " .. cv[2], nil)
        end)
    end
    calmed = {}
end

local function Norm(a) a = a % 360; if a > 180 then a = a - 360 end; return a end
local function DebugExtUpdate()
    local dcc = DebugController()
    if not Valid(dcc) then return end
    local pawn = nil
    pcall(function() pawn = dcc.Pawn end)
    if not Valid(pawn) then pcall(function() pawn = dcc:GetSpectatorPawn() end) end
    if not Valid(pawn) then return end
    local now = os.clock()
    if not dbgReady then
        CalmDebugCam(dcc, pawn)
        local l = pawn:K2_GetActorLocation()
        local r = dcc:GetControlRotation()
        state.x, state.y, state.z = l.X, l.Y, l.Z
        state.pitch, state.yaw = Norm(r.Pitch), r.Yaw
        state.tyaw, state.tpitch, state.vx, state.vy, state.vz = nil, nil, 0, 0, 0
        dbgReady, dbgLast = true, now
        diag.x = nil
        ext.seq, ext.yaw, ext.pitch = -1, nil, nil
    end
    local dt = math.min(0.1, math.max(0, now - (dbgLast or now))); dbgLast = now
    if dt <= 0 then return end
    -- check whether something else moved / turned the camera since our last frame (logged every few seconds)
    if diag.x then
        pcall(function()
            local l, r = pawn:K2_GetActorLocation(), dcc:GetControlRotation()
            local d = math.sqrt((l.X - diag.x) ^ 2 + (l.Y - diag.y) ^ 2 + (l.Z - diag.z) ^ 2)
            local a = math.max(math.abs(Norm(r.Yaw - diag.yaw)), math.abs(Norm(r.Pitch - diag.pitch)))
            if d > 0.5 then diag.moved = diag.moved + 1; diag.maxd = math.max(diag.maxd, d) end
            if a > 0.05 then diag.turned = diag.turned + 1; diag.maxa = math.max(diag.maxa, a) end
        end)
    end
    diag.frames = diag.frames + 1
    if now - diag.at > 3 then
        if diag.moved > 0 or diag.turned > 0 then
            Log(string.format("editing camera: %d frames, moved by the game %d times (up to %.0f), turned by the game %d times (up to %.1f deg)",
                diag.frames, diag.moved, diag.maxd, diag.turned, diag.maxa))
        end
        diag.at, diag.frames, diag.moved, diag.turned, diag.maxd, diag.maxa = now, 0, 0, 0, 0, 0
    end
    ExtStep(dt)
    local hit = {}
    pcall(function() pawn:K2_SetActorLocation(Vec(state.x, state.y, state.z), false, hit, true) end)
    pcall(function() dcc:SetControlRotation(Rot(state.pitch, state.yaw, 0)) end)
    diag.x, diag.y, diag.z, diag.yaw, diag.pitch = state.x, state.y, state.z, state.yaw, state.pitch
    -- some games (South of Midnight) keep shaking the camera and feed their own mouse / stick look into the
    -- debug camera too, so the view wobbles between the two: keep both held back every frame
    -- (these are counters in Unreal, so only add one when it isn't held back already)
    pcall(function() if not dcc:IsLookInputIgnored() then dcc:SetIgnoreLookInput(true) end end)
    pcall(function() if not dcc:IsMoveInputIgnored() then dcc:SetIgnoreMoveInput(true) end end)
    if Valid(dbgPcm) then pcall(function() dbgPcm:StopAllCameraShakes(true) end) end
    -- South of Midnight's debug camera shows the game's hand pointer and steers the view with it: keep it hidden
    -- (Unreal's normal debug camera has no pointer, and then the editor holds back its mouse look)
    pcall(function() if dcc.bShowMouseCursor then dcc.bShowMouseCursor = false end end)
    pcall(function() if dcc.bEnableMouseOverEvents then dcc.bEnableMouseOverEvents = false end end)
    pcall(function() if dcc.bEnableClickEvents then dcc.bEnableClickEvents = false end end)
end
-- Unreal's F key: put the flying camera in front of a point, looking at it the way it looks now, dist away
function F.FocusOn(x, y, z, dist)
    if not F.active or not state.x then return false end
    local p, yw = math.rad(state.pitch or 0), math.rad(state.yaw or 0)
    local fx, fy, fz = math.cos(p) * math.cos(yw), math.cos(p) * math.sin(yw), math.sin(p)
    state.x, state.y, state.z = x - fx * dist, y - fy * dist, z - fz * dist
    state.vx, state.vy, state.vz = 0, 0, 0
    return true
end
-- the launcher stopped flying the camera (editor closed, freecam stays on): give it back its own input / blur
function F.EndExt()
    pcall(RestoreCalm)
    dbgReady = false
end
function F.ResetExt()
    dbgReady = false; ext.seq, ext.yaw, ext.pitch = -1, nil, nil
    state.tyaw, state.tpitch, state.vx, state.vy, state.vz = nil, nil, 0, 0, 0
end

-- Called every frame from main.lua's single game-thread job.
F.hooked = false            -- true once Unreal's per-frame camera update is driving us
function F.Frame()
    if not F.active then return end
    if F.kind == "builtin" then pcall(BuiltinUpdate, 1 / 60)
    elseif F.kind == "debug" and F.ext then
        local ok, err = pcall(DebugExtUpdate)
        if not ok then Log("editing camera: " .. tostring(err)) end
    end
end

-- ---------------------------------------------------------------- public API (call from the game thread)
function F.Toggle(want, forceKind)
    if want == nil then want = not F.active end
    if not want then
        if not F.active then return true, "Freecam is already off." end
        if F.kind == "debug" then DebugCamOff() else BuiltinOff() end
        pcall(RestoreCalm)
        dbgReady = false
        F.active, F.kind = false, "off"
        return true, "Freecam off"
    end
    if F.active then return true, "Freecam is already on." end
    local order
    if forceKind then order = { forceKind }
    elseif F.cfg.mode == "debug" then order = { "debug" }
    elseif F.cfg.mode == "builtin" then order = { "builtin" }
    else order = debugUnavailable and { "builtin" } or { "debug", "builtin" } end
    local lastErr = "Couldn't start freecam."
    for _, kind in ipairs(order) do
        local ok, a, b = pcall(kind == "debug" and DebugCamOn or BuiltinOn)
        if ok and a then
            F.active, F.kind = true, kind
            mouseOK = true
            ext.seq, ext.yaw, ext.pitch = -1, nil, nil
            state.tyaw, state.tpitch, state.vx, state.vy, state.vz = nil, nil, 0, 0, 0
            lastClock = nil
            return true, kind == "debug" and "Freecam on (Unreal debug camera)" or "Freecam on (simple freecam)"
        end
        local err = ok and b or a
        if kind == "debug" and err == "unavailable" then
            debugUnavailable = true
            err = "This game has Unreal's debug camera removed."
        end
        lastErr = tostring(err)
    end
    return false, lastErr
end

-- Moves the player character to where the camera is, then turns freecam off.
function F.TeleportPlayer()
    if not F.active then return false, "Turn freecam on and fly to where you want to go first." end
    local x, y, z, yaw
    if F.kind == "builtin" then
        x, y, z, yaw = state.x, state.y, state.z, state.yaw
    else
        local pc = F.ActivePC()
        local pcm = pc.PlayerCameraManager
        local l, r = pcm:GetCameraLocation(), pcm:GetCameraRotation()
        x, y, z, yaw = l.X, l.Y, l.Z, r.Yaw
    end
    F.Toggle(false)
    ExecuteInGameThread(function()
        local pawn = UEHelpers.GetPlayer()
        if not Valid(pawn) then Log("No player character to teleport."); return end
        local ok = pcall(function() pawn:K2_TeleportTo(Vec(x, y, z), Rot(0, yaw, 0)) end)
        Log(ok and string.format("Teleported player to %.0f, %.0f, %.0f", x, y, z) or "Teleport failed")
    end)
    return true, string.format("Teleporting player to %.0f, %.0f, %.0f", x, y, z)
end

-- Refreshes F.camText / F.rotText (game thread).
function F.UpdateCameraInfo()
    if F.active and F.kind == "debug" then
        local dcc = DebugController()
        if dcc then
            local ok, sc = pcall(function() return dcc.SpeedScale end)
            if ok and type(sc) == "number" and sc > 0 then F.speed = sc end
        end
    end
    if F.active and F.kind == "builtin" and state.x then
        F.camText = string.format("%.1f,%.1f,%.1f", state.x, state.y, state.z)
        F.rotText = string.format("%.1f,%.1f", state.pitch, state.yaw)
        return
    end
    local pc = F.ActivePC()
    if not Valid(pc) then F.camText, F.rotText = "", ""; return end
    local pcm = pc.PlayerCameraManager
    if not Valid(pcm) then return end
    local ok, l = pcall(function() return pcm:GetCameraLocation() end)
    local ok2, r = pcall(function() return pcm:GetCameraRotation() end)
    if ok and l then F.camText = string.format("%.1f,%.1f,%.1f", l.X, l.Y, l.Z) end
    if ok2 and r then F.rotText = string.format("%.1f,%.1f", r.Pitch, r.Yaw) end
end

-- A new level destroys our camera / the debug controller.
function F.OnMapChanged()
    F.active, F.kind = false, "off"
    state = {}
end

function F.ApplyConfig(text)
    for k, v in string.gmatch(text or "", "([%w_]+)=([^\r\n]*)") do
        if k == "enabled" then F.cfg.enabled = (v == "1")
        elseif k == "key" then F.cfg.key = v
        elseif k == "mode" then F.cfg.mode = v
        elseif k == "hidetext" then F.cfg.hideText = (v == "1")
        elseif k == "speed" then
            local sp = tonumber(v) or 1.0
            F.cfg.speed = sp
            if lastCfgSpeed == nil or math.abs(sp - lastCfgSpeed) > 0.001 then
                lastCfgSpeed = sp
                if math.abs(sp - F.speed) > 0.001 then ExecuteInGameThread(function() F.SetSpeed(sp) end) end
            end
        end
    end
end

-- Speed keys while freecam is on: + / - (main keyboard or numpad)
function F.RegisterSpeedKeys()
    local function bind(name, factor)
        if Key[name] then
            RegisterKeyBind(Key[name], function()
                if F.active then ExecuteInGameThread(function() F.ChangeSpeed(factor) end) end
            end)
        end
    end
    bind("OEM_PLUS", 1.5); bind("ADD", 1.5)
    bind("OEM_MINUS", 1 / 1.5); bind("SUBTRACT", 1 / 1.5)
end

-- Hotkeys: register every allowed key once and react only to the one chosen in the launcher,
-- so changing the key takes effect without restarting the game.
F.HotkeyNames = { "INS", "HOME", "END", "DEL", "PAGE_UP", "PAGE_DOWN", "F5", "F6", "F7", "F8", "F9", "F11", "F12" }
function F.RegisterHotkeys()
    for _, name in ipairs(F.HotkeyNames) do
        local code = Key[name]
        if code then
            RegisterKeyBind(code, function()
                if F.cfg.enabled and F.cfg.key == name then
                    ExecuteInGameThread(function()
                        local ok, msg = F.Toggle()
                        Log(msg)
                    end)
                end
            end)
        end
    end
end

return F
