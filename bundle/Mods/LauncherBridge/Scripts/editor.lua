-- In-game editor backend for the UE4SS Launcher overlay (F2).
-- The overlay sends ed_* commands; this module applies them inside the game and writes
-- editor_state.txt (player + selection, ~3x a second), props.txt (selected actor's values, ~1x a second)
-- and actors.txt (on request). All engine calls use the parameter types the engine expects.
local UEHelpers = require("UEHelpers")

local E = { open = false }
local Dir = nil
local Freecam = nil
local selected, selectedPath = nil, ""
local actorCache = {}            -- path -> actor from the last list
local msgs, msgId = {}, 0

local function Log(m) print("[LauncherBridge] [Editor] " .. tostring(m) .. "\n") end
local function Valid(o)
    if o == nil then return false end
    local ok, v = pcall(function() return o:IsValid() end)
    return ok and v == true
end
local function Str(v)
    if v == nil then return "" end
    local t = type(v)
    if t == "string" or t == "number" or t == "boolean" then return tostring(v) end
    local ok, s = pcall(function() return v:ToString() end)
    if ok and s ~= nil then return tostring(s) end
    return tostring(v)
end
local function Clean(s) return (tostring(s or ""):gsub("[\t\r\n]", " ")) end
local function Vec(x, y, z) return { X = x, Y = y, Z = z } end
local function Rot(p, y, r) return { Pitch = p, Yaw = y, Roll = r } end
local function Num3(s)
    local a, b, c = tostring(s or ""):match("^%s*([%-%d%.eE]+)%s*,%s*([%-%d%.eE]+)%s*,%s*([%-%d%.eE]+)")
    if not a then return nil end
    return tonumber(a), tonumber(b), tonumber(c)
end
local function F3(v) if not v then return "0,0,0" end return string.format("%.2f,%.2f,%.2f", v.X or 0, v.Y or 0, v.Z or 0) end
local function R3(v) if not v then return "0,0,0" end return string.format("%.2f,%.2f,%.2f", v.Pitch or 0, v.Yaw or 0, v.Roll or 0) end

-- Loads a game asset from its files. The engine's own loader (KismetSystemLibrary.LoadAsset_Blocking) works in
-- games where UE4SS's LoadAsset doesn't (it needs the asset registry, which games like Hogwarts Legacy cook
-- without most assets). UE4SS's is still tried after it.
local UE4SSLoadAsset = LoadAsset
local function LoadAsset(path)
    if not path or path == "" then return nil end
    if not path:find("%.") then path = path .. "." .. (path:match("([^/]+)$") or "") end
    local ok, r = pcall(function()
        local ksl = StaticFindObject("/Script/Engine.Default__KismetSystemLibrary")
        local sp = ksl:MakeSoftObjectPath(path)
        local ref = ksl:Conv_SoftObjPathToSoftObjRef(sp)
        return ksl:LoadAsset_Blocking(ref)
    end)
    if ok and r ~= nil and Valid(r) then return r end
    if type(UE4SSLoadAsset) == "function" then
        local ok2, r2 = pcall(UE4SSLoadAsset, path)
        if ok2 then return r2 end
    end
    return nil
end
local function Msg(m, isErr)
    msgId = msgId + 1
    table.insert(msgs, string.format("%d\t%s\t%s", msgId, isErr and "err" or "ok", Clean(m)))
    if #msgs > 12 then table.remove(msgs, 1) end
    Log(m)
end

local function WriteFile(path, data)
    local tmp = path .. ".tmp"
    local f = io.open(tmp, "w")
    if not f then return false end
    f:write(data); f:close()
    os.remove(path)
    return os.rename(tmp, path)
end

-- The real player controller/pawn (not the debug camera's controller).
local Fast = require("fast")
local slowPC, slowAt = nil, -100
local function PlayerPC()
    local pc = Fast.PlayerPC()   -- straight from the engine (searching every object made big games stutter)
    if Valid(pc) then
        local okp, pawn = pcall(function() return pc.Pawn end)
        if okp and Valid(pawn) then return pc, pawn end
    end
    -- some games keep the player elsewhere: search, but at most every 2 seconds, and keep the answer
    if Valid(slowPC) then
        local okp, pawn = pcall(function() return slowPC.Pawn end)
        if okp and Valid(pawn) then return slowPC, pawn end
    end
    if os.clock() - slowAt >= 2 then
        slowAt = os.clock()
        for _, c in ipairs(FindAllOf("PlayerController") or {}) do
            if Valid(c) and not Fast.IsDebugPC(c) then
                local okp, pawn = pcall(function() return c.Pawn end)
                if okp and Valid(pawn) then slowPC = c; return c, pawn end
            end
        end
    end
    if Valid(pc) then return pc, pc.Pawn end
    return nil, nil
end
local function World()
    local pc = PlayerPC()
    if Valid(pc) then return pc:GetWorld() end
    return UEHelpers.GetWorld()
end
local function ViewPC()
    if Freecam then return Freecam.ActivePC() end
    return PlayerPC()
end
local function Movement(pawn)
    if not Valid(pawn) then return nil end
    local ok, m = pcall(function() return pawn.CharacterMovement end)
    if ok and Valid(m) then return m end
    return nil
end
local function ObjectPath(a)
    local ok, full = pcall(function() return a:GetFullName() end)
    if not ok or not full then return "" end
    return full:match("^%S+%s+(.+)$") or full
end
local function IsActor(o)
    local ok, r = pcall(function() return o:IsA("/Script/Engine.Actor") end)
    return ok and r == true
end
-- Turns a component / instance / sub-object into the actor that owns it.
local function ToActor(o)
    for _ = 1, 8 do
        if not Valid(o) then return nil end
        if IsActor(o) then return o end
        local ok, own = pcall(function() return o:GetOwner() end)
        if ok and Valid(own) then o = own
        else
            local ok2, outer = pcall(function() return o:GetOuter() end)
            if ok2 and Valid(outer) then o = outer else return nil end
        end
    end
    return nil
end
local function FindActor(path)
    local a = actorCache[path]
    if Valid(a) then return ToActor(a) end
    if selectedPath == path and Valid(selected) then return ToActor(selected) end
    local ok, o = pcall(function() return StaticFindObject(path) end)
    if ok and Valid(o) then return ToActor(o) end
    return nil
end

-- ---------------------------------------------------------------- state files
local function PlayerSection()
    local out = {}
    local pc, pawn = PlayerPC()
    if not Valid(pawn) then table.insert(out, "pawn="); return out end
    table.insert(out, "pawn=" .. Clean(Str(pawn:GetClass():GetFName())))
    if E.SwappedTo and E.SwappedTo() then table.insert(out, "swapped=" .. Clean(E.SwappedTo()))
    elseif E.PossessActive and E.PossessActive() then table.insert(out, "playingas=" .. Clean(Str(pawn:GetFName()))) end
    if E.LookingAs and E.LookingAs() then table.insert(out, "lookingas=" .. Clean(E.LookingAs())) end
    local okL, l = pcall(function() return pawn:K2_GetActorLocation() end)
    local okR, r = pcall(function() return pawn:K2_GetActorRotation() end)
    table.insert(out, "loc=" .. (okL and F3(l) or "0,0,0"))
    table.insert(out, "rot=" .. (okR and R3(r) or "0,0,0"))
    local m = Movement(pawn)
    if m then
        local function num(k) local ok, v = pcall(function() return m[k] end); return ok and type(v) == "number" and v or 0 end
        table.insert(out, string.format("walk=%.2f", num("MaxWalkSpeed")))
        table.insert(out, string.format("jump=%.2f", num("JumpZVelocity")))
        table.insert(out, string.format("gravity=%.3f", num("GravityScale")))
        table.insert(out, string.format("mode=%d", math.floor(num("MovementMode"))))
    end
    local okD, dmg = pcall(function() return pawn.bCanBeDamaged end)
    table.insert(out, "god=" .. ((okD and dmg == false) and "1" or "0"))
    local okC, col = pcall(function() return pawn:GetActorEnableCollision() end)
    table.insert(out, "ghost=" .. ((okC and col == false) and "1" or "0"))
    local w = World()
    local okT, td = pcall(function() return UEHelpers.GetGameplayStatics():GetGlobalTimeDilation(w) end)
    table.insert(out, string.format("timescale=%.3f", (okT and type(td) == "number") and td or 1))
    local vpc = ViewPC()
    local okF, fov = pcall(function() return vpc.PlayerCameraManager:GetFOVAngle() end)
    table.insert(out, string.format("fov=%.1f", (okF and type(fov) == "number") and fov or 90))
    return out
end

local function SelectedSection()
    if not Valid(selected) then
        selected = nil
        return { "sel=" }
    end
    local out = { "sel=" .. selectedPath }
    table.insert(out, "selname=" .. Clean(Str(selected:GetFName())))
    table.insert(out, "selclass=" .. Clean(Str(selected:GetClass():GetFName())))
    local okL, l = pcall(function() return selected:K2_GetActorLocation() end)
    local okR, r = pcall(function() return selected:K2_GetActorRotation() end)
    local okS, s = pcall(function() return selected:GetActorScale3D() end)
    table.insert(out, "selloc=" .. (okL and F3(l) or "0,0,0"))
    table.insert(out, "selrot=" .. (okR and R3(r) or "0,0,0"))
    table.insert(out, "selscale=" .. (okS and F3(s) or "1,1,1"))
    local okH, h = pcall(function() return selected.bHidden end)
    table.insert(out, "selhidden=" .. ((okH and h == true) and "1" or "0"))
    -- centre of the object (middle of its bounding box), used for the gizmo and as the rotation pivot
    local cx3, cy3, cz3 = nil, nil, nil
    pcall(function()
        local origin, extent = {}, {}
        local ok = pcall(function() selected:GetActorBounds(false, origin, extent, false) end)
        if not ok then origin, extent = {}, {}; selected:GetActorBounds(false, origin, extent) end
        if origin.X and extent.X and (extent.X > 0 or extent.Y > 0 or extent.Z > 0) and extent.X < 1e6 then
            cx3, cy3, cz3 = origin.X, origin.Y, origin.Z
        end
    end)
    if okL and l and not cx3 then cx3, cy3, cz3 = l.X, l.Y, l.Z end
    if cx3 then table.insert(out, string.format("selcenter=%.2f,%.2f,%.2f", cx3, cy3, cz3)) end
    -- where it is on screen (0-1) plus where its X/Y/Z axes point, so the overlay can draw the gizmo
    if cx3 then
        local l = { X = cx3, Y = cy3, Z = cz3 }
        pcall(function()
            local vpc = ViewPC()
            local size = {}
            vpc:GetViewportSize(size, size)
            local w, hh = tonumber(size.SizeX) or 0, tonumber(size.SizeY) or 0
            if w <= 0 or hh <= 0 then return end
            local function proj(x, y, z)
                local scr = {}
                local ok, vis = pcall(function() return vpc:ProjectWorldLocationToScreen({ X = x, Y = y, Z = z }, scr, false) end)
                if ok and vis and scr.X then return scr.X / w, scr.Y / hh end
                return nil
            end
            local cx, cy = proj(l.X, l.Y, l.Z)
            if not cx then return end
            table.insert(out, string.format("selscreen=%.4f,%.4f", cx, cy))
            -- axis length in world units scaled by distance, so the gizmo stays a similar size on screen
            local pcm = vpc.PlayerCameraManager
            local cam = pcm:GetCameraLocation()
            pcall(function()
                local cr = pcm:GetCameraRotation()
                table.insert(out, string.format("selcam=%.3f,%.3f,%.3f,%.4f,%.4f,%.4f,%.4f,%d,%d", cam.X, cam.Y, cam.Z, cr.Pitch, cr.Yaw, cr.Roll, pcm:GetFOVAngle(), w, hh))
            end)
            local d = math.sqrt((l.X - cam.X) ^ 2 + (l.Y - cam.Y) ^ 2 + (l.Z - cam.Z) ^ 2)
            local len = math.max(1, d * 0.16)
            local xx, xy = proj(l.X + len, l.Y, l.Z)
            local yx, yy = proj(l.X, l.Y + len, l.Z)
            local zx, zy = proj(l.X, l.Y, l.Z + len)
            if xx and yx and zx then
                table.insert(out, string.format("gizmo=%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.3f", cx, cy, xx, xy, yx, yy, zx, zy, len))
            end
        end)
    end
    return out
end

function E.WriteState()
    local lines = { "[player]" }
    local ok, p = pcall(PlayerSection)
    if ok then for _, l in ipairs(p) do table.insert(lines, l) end end
    table.insert(lines, "[selected]")
    local ok2, s = pcall(SelectedSection)
    if ok2 then for _, l in ipairs(s) do table.insert(lines, l) end end
    table.insert(lines, "[added]")
    if E.SpawnedLines then
        local ok3, a = pcall(E.SpawnedLines)
        if ok3 then for _, l in ipairs(a) do table.insert(lines, l) end end
    end
    if E.LayoutLine then local okL, l = pcall(E.LayoutLine); if okL and l then table.insert(lines, l) end end
    if E.UndoLine then local okU, l = pcall(E.UndoLine); if okU and l then table.insert(lines, l) end end
    table.insert(lines, "[msgs]")
    for _, m in ipairs(msgs) do table.insert(lines, "msg=" .. m) end
    WriteFile(Dir .. "editor_state.txt", table.concat(lines, "\n") .. "\n")
end

local EDITABLE = { FloatProperty = "float", DoubleProperty = "float", IntProperty = "int", Int64Property = "int",
                   UInt32Property = "int", Int16Property = "int", ByteProperty = "int", BoolProperty = "bool",
                   StrProperty = "str", NameProperty = "name", TextProperty = "text" }

function E.WriteProps()
    if not Valid(selected) then WriteFile(Dir .. "props.txt", "for=\n"); return end
    local lines = { "for=" .. selectedPath }
    -- the models this object is made of (for "Export model"): every visible static / skeletal mesh piece, with
    -- where it sits in the world, plus the object's own transform (the launcher saves the model relative to it)
    local okM, errM = pcall(function()
        local function V(v) return string.format("%.3f,%.3f,%.3f", v.X, v.Y, v.Z) end
        local function Rr(r) return string.format("%.3f,%.3f,%.3f", r.Pitch, r.Yaw, r.Roll) end
        local first = nil
        local seen = {}
        local function add(kind, comp, asset)
            if not Valid(comp) or not Valid(asset) then return end
            local key = tostring(comp:GetAddress())
            if seen[key] then return end
            seen[key] = true
            local okL, l = pcall(function() return comp:K2_GetComponentLocation() end)
            local okR, r = pcall(function() return comp:K2_GetComponentRotation() end)
            local okS, sc = pcall(function() return comp:K2_GetComponentScale() end)
            if not (okL and okR and okS and l and r and sc) then return end
            local path = ObjectPath(asset)
            table.insert(lines, string.format("meshp=%s\t%s\t%s\t%s\t%s", kind, path, V(l), Rr(r), V(sc)))
            if not first then first = kind .. "\t" .. path end
        end
        -- the engine hands arrays back as its own array object, not a Lua table: turn either into a list
        local function List(arr)
            if type(arr) == "table" and rawget(arr, 1) ~= nil then return arr end
            local out = {}
            if arr == nil then return out end
            local ok = pcall(function() arr:ForEach(function(_, e) local v = e:get(); if Valid(v) then table.insert(out, v) end end) end)
            if not ok then pcall(function() for i = 1, #arr do local v = arr[i]; if v and v.get then v = v:get() end; if Valid(v) then table.insert(out, v) end end end) end
            return out
        end
        local function Comps(cls) local okC, arr = pcall(function() return selected:K2_GetComponentsByClass(cls) end); return okC and List(arr) or {} end
        -- the object's main model first (static mesh actors, characters, skeletal mesh actors)
        pcall(function() local c = selected.StaticMeshComponent; if Valid(c) then add("static", c, c.StaticMesh) end end)
        for _, prop in ipairs({ "Mesh", "SkeletalMeshComponent" }) do
            pcall(function()
                local c = selected[prop]
                if Valid(c) then
                    local sk = nil
                    pcall(function() sk = c.SkeletalMesh end)
                    if not Valid(sk) then pcall(function() sk = c.SkinnedAsset end) end
                    add("skeletal", c, sk)
                end
            end)
        end
        local n = 0
        local smcCls = StaticFindObject("/Script/Engine.StaticMeshComponent")
        if Valid(smcCls) then
            for _, c in ipairs(Comps(smcCls)) do
                if n >= 48 then break end
                local okC, cname = pcall(function() return c:GetClass():GetFName():ToString() end)
                local vis = true; pcall(function() vis = c:IsVisible() end)
                if Valid(c) and vis and not (okC and cname:find("Instanced")) then add("static", c, c.StaticMesh); n = n + 1 end
            end
        end
        local skcCls = StaticFindObject("/Script/Engine.SkeletalMeshComponent")
        if Valid(skcCls) then
            for _, c in ipairs(Comps(skcCls)) do
                if n >= 48 then break end
                local vis = true; pcall(function() vis = c:IsVisible() end)
                if Valid(c) and vis then
                    local sk = nil
                    pcall(function() sk = c.SkeletalMesh end)
                    if not Valid(sk) then pcall(function() sk = c.SkinnedAsset end) end
                    add("skeletal", c, sk); n = n + 1
                end
            end
        end
        -- effects on the object (Niagara and the older Cascade particle systems): saved with the model, and put back
        -- when it's placed in a game that has the same effect
        local fxn = 0
        for _, fx in ipairs({ { "/Script/Niagara.NiagaraComponent", "niagara", "Asset" }, { "/Script/Engine.ParticleSystemComponent", "cascade", "Template" } }) do
            local cls = StaticFindObject(fx[1])
            if Valid(cls) then
                for _, c in ipairs(Comps(cls)) do
                    if fxn >= 24 then break end
                    local asset = nil; pcall(function() asset = c[fx[3]] end)
                    if Valid(c) and Valid(asset) then add(fx[2], c, asset); fxn = fxn + 1 end
                end
            end
        end
        -- lights on the object (lamps, neon signs, glowing screens): their settings are saved with the model and
        -- rebuilt in whatever game it's placed in. Spot and rect lights first (a spot light is also a point light).
        local lightn = 0
        for _, L in ipairs({ { "/Script/Engine.SpotLightComponent", "spot" }, { "/Script/Engine.RectLightComponent", "rect" }, { "/Script/Engine.PointLightComponent", "point" } }) do
            local cls = StaticFindObject(L[1])
            if Valid(cls) then
                for _, c in ipairs(Comps(cls)) do
                    if lightn >= 16 then break end
                    local key = Valid(c) and tostring(c:GetAddress()) or nil
                    local vis = true; pcall(function() vis = c:IsVisible() end)
                    if key and not seen[key] and vis then
                        seen[key] = true
                        local function P(n, d) local ok, v = pcall(function() return c[n] end); if ok and type(v) == "number" then return v end; return d end
                        local col = { 255, 255, 255 }
                        pcall(function() local lc = c.LightColor; col = { lc.R, lc.G, lc.B } end)
                        local desc = string.format("%s|%d,%d,%d|%.4f|%.1f|%.2f|%.2f|%.1f|%.1f|%d|%.2f", L[2], col[1] or 255, col[2] or 255, col[3] or 255,
                            P("Intensity", 5000), P("AttenuationRadius", 1000), P("InnerConeAngle", 0), P("OuterConeAngle", 44), P("SourceWidth", 64), P("SourceHeight", 64),
                            math.floor(P("IntensityUnits", 0)), P("SourceRadius", 0))
                        local okL, l = pcall(function() return c:K2_GetComponentLocation() end)
                        local okR, r = pcall(function() return c:K2_GetComponentRotation() end)
                        if okL and okR and l and r then
                            table.insert(lines, string.format("meshp=light\t%s\t%s\t%s\t1.000,1.000,1.000", desc, V(l), Rr(r)))
                            lightn = lightn + 1
                        end
                    end
                end
            end
        end
        if first then
            table.insert(lines, "mesh=" .. first)
            local l = selected:K2_GetActorLocation(); local r = selected:K2_GetActorRotation(); local sc = selected:GetActorScale3D()
            table.insert(lines, string.format("actorxf=%s\t%s\t%s", V(l), Rr(r), V(sc)))
        end
    end)
    if not okM then print("[LauncherBridge] [Editor] couldn't list the object's models: " .. tostring(errM) .. "\n") end
    local count = 0
    local cls = selected:GetClass()
    local depth = 0
    while Valid(cls) and depth < 12 and count < 600 do
        local cname = Clean(Str(cls:GetFName()))
        pcall(function()
            cls:ForEachProperty(function(prop)
                local okT, tn = pcall(function() return prop:GetClass():GetFName():ToString() end)
                local kind = okT and EDITABLE[tn] or nil
                if kind then
                    local name = prop:GetFName():ToString()
                    local okV, v = pcall(function() return selected[name] end)
                    if okV and v ~= nil then
                        local val
                        if kind == "bool" then val = (v == true) and "1" or "0"
                        elseif kind == "float" then val = string.format("%.4f", tonumber(v) or 0)
                        elseif kind == "int" then val = string.format("%d", math.floor(tonumber(v) or 0))
                        else val = Clean(Str(v)) end
                        table.insert(lines, string.format("p=%s\t%s\t%s\t%s", cname, name, kind, val))
                        count = count + 1
                    end
                end
                return count >= 600
            end)
        end)
        local okS, sup = pcall(function() return cls:GetSuperStruct() end)
        if not okS or not Valid(sup) then break end
        cls = sup
        depth = depth + 1
        if cname == "Actor" then break end
    end
    WriteFile(Dir .. "props.txt", table.concat(lines, "\n") .. "\n")
end

-- ---------------------------------------------------------------- actor list / picking
local function ViewLocation()
    local vpc = ViewPC()
    local ok, l = pcall(function() return vpc.PlayerCameraManager:GetCameraLocation() end)
    if ok and l then return l.X, l.Y, l.Z end
    local _, pawn = PlayerPC()
    if Valid(pawn) then local l2 = pawn:K2_GetActorLocation(); return l2.X, l2.Y, l2.Z end
    return 0, 0, 0
end

function E.ListActors(filter)
    filter = (filter or ""):lower()
    local cx, cy, cz = ViewLocation()
    local all = FindAllOf("Actor") or {}
    local rows = {}
    actorCache = {}
    local anyGone = next(E.softDeleted) ~= nil
    for _, a in ipairs(all) do
        if Valid(a) and not (anyGone and E.softDeleted[ObjectPath(a)]) then
            local okN, name = pcall(function() return a:GetFName():ToString() end)
            local okC, cls = pcall(function() return a:GetClass():GetFName():ToString() end)
            name, cls = okN and name or "?", okC and cls or "?"
            if filter == "" or name:lower():find(filter, 1, true) or cls:lower():find(filter, 1, true) then
                local okL, l = pcall(function() return a:K2_GetActorLocation() end)
                if okL and l then
                    local dx, dy, dz = l.X - cx, l.Y - cy, l.Z - cz
                    table.insert(rows, { a = a, name = name, cls = cls, d = math.sqrt(dx * dx + dy * dy + dz * dz) })
                end
            end
        end
    end
    table.sort(rows, function(x, y) return x.d < y.d end)
    local lines = { "filter=" .. Clean(filter), "total=" .. #rows }
    for i = 1, math.min(#rows, 500) do
        local r = rows[i]
        local path = ObjectPath(r.a)
        actorCache[path] = r.a
        table.insert(lines, string.format("a=%s\t%s\t%s\t%.0f", path, Clean(r.name), Clean(r.cls), r.d))
    end
    WriteFile(Dir .. "actors.txt", table.concat(lines, "\n") .. "\n")
    Msg(string.format("Found %d actors%s", #rows, filter ~= "" and (" matching \"" .. filter .. "\"") or ""))
end

local function HitActor(hit)
    local function try(f) local ok, v = pcall(f); if ok and Valid(v) then return ToActor(v) end end
    return try(function() return hit.Actor:Get() end)
        or try(function() return hit.HitObjectHandle.Actor:Get() end)
        or try(function() return hit.HitObjectHandle.ReferenceObject:Get() end)
        or try(function() return hit.Component:Get() end)
end

local function Vadd(a, b, k) return { X = a.X + b.X * k, Y = a.Y + b.Y * k, Z = a.Z + b.Z * k } end

-- Line trace (same call pattern as UE4SS's LineTraceMod); tries the Visibility then the Camera channel.
local function TraceActor(start, finish)
    local vpc = ViewPC()
    local _, pawn = PlayerPC()
    local ctx = Valid(pawn) and pawn or vpc
    local color = { R = 0, G = 0, B = 0, A = 0 }
    for _, channel in ipairs({ 0, 1 }) do
        local hit = {}
        local ok, wasHit = pcall(function()
            return UEHelpers.GetKismetSystemLibrary():LineTraceSingle(ctx, start, finish, channel, false, {}, 0, hit, true, color, color, 0.0)
        end)
        if ok and wasHit then
            local a = HitActor(hit)
            if a then return a end
        end
    end
    return nil
end

local function SelectHit(a)
    if not a then Msg("Nothing to select there.", true); return end
    E.Select(ObjectPath(a), a)
end

-- Selects whatever is in the middle of the screen.
function E.Pick()
    local pcm = ViewPC().PlayerCameraManager
    local start = pcm:GetCameraLocation()
    local fwd = UEHelpers.GetKismetMathLibrary():GetForwardVector(pcm:GetCameraRotation())
    SelectHit(TraceActor({ X = start.X, Y = start.Y, Z = start.Z }, Vadd(start, fwd, 100000)))
end

-- Selects whatever is under the mouse. nx, ny = position on the game's screen from 0 to 1.
function E.PickAt(nx, ny)
    local vpc = ViewPC()
    local size = {}
    vpc:GetViewportSize(size, size)
    local w, h = tonumber(size.SizeX) or 0, tonumber(size.SizeY) or 0
    if w <= 0 or h <= 0 then Msg("Couldn't read the game's screen size.", true); return end
    local loc, dir = {}, {}
    local ok = vpc:DeprojectScreenPositionToWorld(nx * w, ny * h, loc, dir)
    if not ok or not loc.X or not dir.X then Msg("Couldn't work out what's under the mouse.", true); return end
    SelectHit(TraceActor({ X = loc.X, Y = loc.Y, Z = loc.Z }, Vadd(loc, dir, 100000)))
end

function E.Select(path, obj)
    local a = obj and ToActor(obj) or FindActor(path)
    if a and obj then path = ObjectPath(a) end
    if not a then Msg("That actor no longer exists.", true); selected, selectedPath = nil, ""; return end
    selected, selectedPath = a, path
    actorCache[path] = a
    Msg("Selected " .. Str(a:GetFName()))
    E.WriteState(); E.WriteProps()
end

-- ---------------------------------------------------------------- edits
local function EnsureMove(pawn)
    local m = Movement(pawn)
    if not m then Msg("This player has no character movement to change.", true) end
    return m
end

local handlers = {}
E.softDeleted = {}     -- path -> true: deleted with the editor but kept (hidden, no collision) so Undo can bring it back

-- While the editor is open the game ignores mouse-look and movement, so the camera stays still.
-- (Many games read the mouse as raw input, which the launcher can't block from outside the game.)
local inputLocked = nil   -- { pc, pawn } we locked
local function LockGameInput(on)
    if on then
        if inputLocked then return end
        local pc, pawn = PlayerPC()
        if not Valid(pc) then return end
        pcall(function() pc:SetIgnoreLookInput(true) end)
        pcall(function() pc:SetIgnoreMoveInput(true) end)
        if Valid(pawn) then pcall(function() pawn:DisableInput(pc) end) end
        inputLocked = { pc = pc, pawn = pawn }
    else
        local l = inputLocked; inputLocked = nil
        if not l then return end
        if Valid(l.pc) then
            pcall(function() l.pc:SetIgnoreLookInput(false) end)
            pcall(function() l.pc:SetIgnoreMoveInput(false) end)
            if Valid(l.pawn) then pcall(function() l.pawn:EnableInput(l.pc) end) end
        end
    end
end
E.LockGameInput = LockGameInput

-- Editing camera: opening the editor (with "fc=1") switches to a freecam that the launcher flies with the
-- controller or right mouse + WASD; closing the editor puts the camera back on the player.
local ourCam = false
local function EditorCamOn()
    if not Freecam then return end
    -- Unreal's debug camera where the game has it (works in most games), else the simple camera.
    -- If a freecam is already on, keep it and fly that one.
    if not Freecam.active then
        local ok, res, msg = pcall(Freecam.Toggle, true)
        if ok and res then ourCam = true
        else Msg("Couldn't start the editing camera: " .. tostring(msg or res), true); return end
        Log("Editing camera: " .. tostring(msg))
    end
    if Freecam.ResetExt then Freecam.ResetExt() end
    Freecam.ext = true
end
local function EditorCamOff()
    if not Freecam then return end
    Freecam.ext = false
    if Freecam.EndExt then pcall(Freecam.EndExt) end
    if ourCam then pcall(Freecam.Toggle, false); ourCam = false end
end
handlers.ed_open = function(arg)
    E.open = true
    LockGameInput(true)
    if (arg or ""):find("fc=1", 1, true) then EditorCamOn() end
    E.WriteState()
end
handlers.ed_close = function() E.open = false; EditorCamOff(); LockGameInput(false) end
handlers.ed_watch = function(arg) E.watch = (arg == "1"); if E.watch then E.WriteState() end end
handlers.ed_editcam = function(arg) if arg == "1" then EditorCamOn() else EditorCamOff() end end
handlers.ed_actors = function(arg) E.ListActors(arg) end
handlers.ed_pick = function() E.Pick() end
handlers.ed_pickat = function(arg)
    local x, y = arg:match("^([%d%.]+),([%d%.]+)$")
    if x then E.PickAt(tonumber(x), tonumber(y)) end
end
handlers.ed_select = function(arg) E.Select(arg) end
handlers.ed_deselect = function() selected, selectedPath = nil, ""; E.WriteState(); E.WriteProps() end

handlers.ed_god = function(arg)
    local _, pawn = PlayerPC(); if not Valid(pawn) then return end
    pawn.bCanBeDamaged = (arg ~= "1")
    Msg(arg == "1" and "God mode on" or "God mode off")
end
handlers.ed_move = function(arg)   -- walk | fly | ghost
    local _, pawn = PlayerPC(); if not Valid(pawn) then return end
    local m = EnsureMove(pawn); if not m then return end
    if arg == "walk" then
        pcall(function() pawn:SetActorEnableCollision(true) end)
        m:SetMovementMode(1, 0)            -- MOVE_Walking
        Msg("Walking")
    else
        m:SetMovementMode(5, 0)            -- MOVE_Flying
        pcall(function() pawn:SetActorEnableCollision(arg ~= "ghost") end)
        Msg(arg == "ghost" and "Ghost: flying through walls" or "Flying")
    end
end
handlers.ed_pset = function(arg)    -- walk|jump|gravity <value>
    local k, v = arg:match("^(%w+)\t(.+)$"); v = tonumber(v)
    if not k or not v then return end
    local _, pawn = PlayerPC(); local m = EnsureMove(pawn); if not m then return end
    local map = { walk = "MaxWalkSpeed", jump = "JumpZVelocity", gravity = "GravityScale" }
    if map[k] then m[map[k]] = v end
    if k == "walk" then pcall(function() m.MaxFlySpeed = v end) end
end
handlers.ed_timescale = function(arg)
    local v = tonumber(arg) or 1
    UEHelpers.GetGameplayStatics():SetGlobalTimeDilation(World(), v)
end
handlers.ed_fov = function(arg)
    local v = tonumber(arg) or 90
    local vpc = ViewPC()
    if not pcall(function() vpc:FOV(v) end) then
        pcall(function() UEHelpers.GetKismetSystemLibrary():ExecuteConsoleCommand(World(), "fov " .. v, vpc) end)
    end
end
handlers.ed_tp = function(arg)
    local x, y, z = Num3(arg); if not x then return end
    local _, pawn = PlayerPC(); if not Valid(pawn) then return end
    local yaw = 0; pcall(function() yaw = pawn:K2_GetActorRotation().Yaw end)
    pawn:K2_TeleportTo(Vec(x, y, z), Rot(0, yaw, 0))
    Msg(string.format("Teleported to %.0f, %.0f, %.0f", x, y, z))
end
handlers.ed_console = function(arg)
    local pc = PlayerPC()
    UEHelpers.GetKismetSystemLibrary():ExecuteConsoleCommand(World(), arg, pc)
    Msg("> " .. arg)
end

-- ---------------------------------------------------------------- developer console, cheats & menus
local devWidgets = {}       -- class path -> widget we put on screen
local ENGINE_MODULES = { "/Script/Engine.", "/Script/CoreUObject.", "/Script/UMG.", "/Script/AIModule.", "/Script/EngineSettings.",
                         "/Script/GameplayTags.", "/Script/Slate.", "/Script/SlateCore.", "/Script/InputCore.", "/Script/EnhancedInput." }
local function FullName(o) local ok, n = pcall(function() return o:GetFullName() end); return ok and tostring(n) or "" end
local function IsEngineClass(c)
    local p = FullName(c)
    for _, m in ipairs(ENGINE_MODULES) do if p:find(m, 1, true) then return true end end
    return false
end
local function EnsureCheatManager()
    local pc = PlayerPC(); if not Valid(pc) then return nil end
    local cm; pcall(function() cm = pc.CheatManager end)
    if Valid(cm) then return cm end
    local cls; pcall(function() cls = pc.CheatClass end)
    if not Valid(cls) then cls = StaticFindObject("/Script/Engine.CheatManager") end
    if not Valid(cls) then return nil end
    pcall(function() cm = StaticConstructObject(cls, pc) end)
    if not Valid(cm) then return nil end
    pcall(function() pc.CheatManager = cm end)
    pcall(function() cm:ReceiveInitCheatManager() end)
    return cm
end
local function Engine()
    local ok, e = pcall(function() return FindFirstOf("GameEngine") end)
    if ok and Valid(e) then return e end
    ok, e = pcall(function() return FindFirstOf("Engine") end)
    return ok and Valid(e) and e or nil
end
local function EnsureConsole()
    local eng = Engine(); if not eng then return false end
    local gv; pcall(function() gv = eng.GameViewport end)
    if not Valid(gv) then return false end
    local con; pcall(function() con = gv.ViewportConsole end)
    if Valid(con) then return true end
    local cls; pcall(function() cls = eng.ConsoleClass end)
    if not Valid(cls) then cls = StaticFindObject("/Script/Engine.Console") end
    if not Valid(cls) then return false end
    pcall(function() con = StaticConstructObject(cls, gv) end)
    if not Valid(con) then return false end
    pcall(function() gv.ViewportConsole = con end)
    return true
end
local function ConsoleKeys()
    local keys = {}
    pcall(function()
        local s = StaticFindObject("/Script/Engine.Default__InputSettings")
        local arr = s.ConsoleKeys
        for i = 1, #arr do table.insert(keys, arr[i].KeyName:ToString()) end
    end)
    return table.concat(keys, ", ")
end
-- the game's own console cheats: exec functions on the objects the console talks to (engine ones are offered as buttons instead)
local function ExecFuncs(obj, where, out, seen)
    if not Valid(obj) then return end
    local cls; pcall(function() cls = obj:GetClass() end)
    local guard = 0
    while Valid(cls) and guard < 30 do
        guard = guard + 1
        if not IsEngineClass(cls) then
            pcall(function() cls:ForEachFunction(function(f)
                local okF, flags = pcall(function() return f:GetFunctionFlags() end)
                if okF and type(flags) == "number" and (flags & 0x200) ~= 0 then        -- FUNC_Exec
                    local name = f:GetFName():ToString()
                    if not seen[name:lower()] then
                        seen[name:lower()] = true
                        local params = {}
                        pcall(function() f:ForEachProperty(function(prop)
                            local isRet = false
                            pcall(function() isRet = prop:HasAnyPropertyFlags(0x400) end)   -- CPF_ReturnParm
                            if not isRet then
                                local t = prop:GetClass():GetFName():ToString():gsub("Property$", "")
                                table.insert(params, prop:GetFName():ToString() .. ":" .. t)
                            end
                        end) end)
                        table.insert(out, { name = name, where = where, params = table.concat(params, " ") })
                    end
                end
            end) end)
        end
        local okS, sup = pcall(function() return cls:GetSuperStruct() end)
        cls = okS and sup or nil
    end
end
local function ClassFor(path)
    -- accepts a class path (/Game/UI/WBP_Debug.WBP_Debug_C) or a package path (/Game/UI/WBP_Debug)
    local tries = { path }
    if not path:find("%.") then
        local n = path:match("([^/]+)$") or ""
        tries = { path .. "." .. n .. "_C", path .. "." .. n }
    end
    for pass = 1, 2 do
        for _, p in ipairs(tries) do
            local ok, c = pcall(function() return StaticFindObject(p) end)
            if ok and Valid(c) then return c end
        end
        if pass == 1 then pcall(function() LoadAsset((path:gsub("%..*$", ""))) end) end
    end
    return nil
end
-- The game's own cheat managers (subclasses of Unreal's CheatManager), loaded in memory. Looking through every
-- class takes a moment in big games, so the list is kept for a minute.
E.cheatClassAt = -1000
function E.CheatClasses()
    if E.cheatClassCache and os.clock() - E.cheatClassAt < 60 then return E.cheatClassCache end
    local base = StaticFindObject("/Script/Engine.CheatManager")
    local out, seen = {}, {}
    if Valid(base) then
        for _, kind in ipairs({ "Class", "BlueprintGeneratedClass" }) do
            pcall(function()
                for _, c in ipairs(FindAllOf(kind) or {}) do
                    local ok, child = pcall(function() return c:IsChildOf(base) end)
                    if ok and child then
                        local path = FullName(c):match("%s(%S+)$") or ""
                        if path ~= "" and path ~= "/Script/Engine.CheatManager" and not path:find("Default__", 1, true) and not seen[path] then
                            seen[path] = true
                            table.insert(out, { path = path, name = (c:GetFName():ToString():gsub("_C$", "")) })
                        end
                    end
                end
            end)
        end
    end
    table.sort(out, function(a, b) return a.name:lower() < b.name:lower() end)
    E.cheatClassCache, E.cheatClassAt = out, os.clock()
    return out
end
local function WriteCheats()
    local pc, pawn = PlayerPC()
    local lines = {}
    local cm = EnsureCheatManager()
    if cm then table.insert(lines, "cheatmanager=1") end
    if EnsureConsole() then table.insert(lines, "console=1") end
    table.insert(lines, "consolekeys=" .. Clean(ConsoleKeys()))
    local list, seen = {}, {}
    ExecFuncs(cm, "Cheat manager", list, seen)
    ExecFuncs(pc, "Player controller", list, seen)
    ExecFuncs(pawn, "Player character", list, seen)
    pcall(function() ExecFuncs(pc.MyHUD, "HUD", list, seen) end)
    local w = World()
    pcall(function() ExecFuncs(w.AuthorityGameMode, "Game mode", list, seen) end)
    pcall(function() ExecFuncs(w.OwningGameInstance, "Game instance", list, seen) end)
    table.sort(list, function(a, b) return a.name:lower() < b.name:lower() end)
    for _, c in ipairs(list) do table.insert(lines, "cheat=" .. Clean(c.name) .. "\t" .. Clean(c.where) .. "\t" .. Clean(c.params)) end
    -- developer / debug menus already loaded in memory
    local menus = {}
    pcall(function()
        for _, c in ipairs(FindAllOf("WidgetBlueprintGeneratedClass") or {}) do
            local n = c:GetFName():ToString()
            local u = n:upper()
            if u:find("DEBUG") or u:find("CHEAT") or u:find("DEVMENU") or u:find("DEV_MENU") or u:find("DEVELOPER") or u:find("ADMIN")
               or u:find("^W_DEV") or u:find("^WBP_DEV") or u:find("^UI_DEV") or u:find("TESTMENU") then
                local path = FullName(c):match("%s(%S+)$") or ""
                if path ~= "" then menus[path] = n end
            end
        end
    end)
    for path in pairs(devWidgets) do if not menus[path] then menus[path] = path:match("([^/%.]+)$") or path end end
    for path, n in pairs(menus) do
        local open = devWidgets[path] and Valid(devWidgets[path])
        local inVp = false
        if open then pcall(function() inVp = devWidgets[path]:IsInViewport() end) end
        table.insert(lines, "menu=" .. path .. "\t" .. Clean((n:gsub("_C$", ""))) .. "\t" .. (inVp and "1" or "0"))
    end
    -- cheat managers: the one in use + the game's own ones
    local cur = ""
    pcall(function() cur = FullName(cm:GetClass()):match("%s(%S+)$") or "" end)
    table.insert(lines, "cheatcurrent=" .. cur)
    for _, c in ipairs(E.CheatClasses()) do
        table.insert(lines, "cheatclass=" .. c.path .. "\t" .. Clean(c.name) .. "\t" .. (c.path == cur and "1" or "0"))
    end
    WriteFile(Dir .. "cheats.txt", table.concat(lines, "\n") .. "\n")
    return cm ~= nil, #list
end
handlers.ed_devscan = function(arg)
    local okCm, n = WriteCheats()
    if arg ~= "quiet" then
        Msg(okCm and string.format("Cheats are on%s", n > 0 and string.format(" - this game has %d cheats of its own", n) or "")
                 or "Couldn't switch the game's cheats on", not okCm)
    end
end
handlers.ed_cheat = function(arg)            -- like ed_console, but makes sure cheats are switched on first
    EnsureCheatManager()
    local pc = PlayerPC()
    UEHelpers.GetKismetSystemLibrary():ExecuteConsoleCommand(World(), arg, pc)
    Msg("> " .. arg)
end
-- swap the player's cheat manager for another one (usually the game's own, which has its developers' cheats)
handlers.ed_usecheat = function(path)
    local pc = PlayerPC(); if not Valid(pc) then Msg("No player right now.", true); return end
    local cls = ClassFor(path)
    local base = StaticFindObject("/Script/Engine.CheatManager")
    local okC, child = pcall(function() return cls:IsChildOf(base) end)
    if not cls or not okC or not child then Msg("That isn't a cheat manager the game can use.", true); return end
    local cm; pcall(function() cm = StaticConstructObject(cls, pc) end)
    if not Valid(cm) then Msg("The game wouldn't make that cheat manager.", true); return end
    pcall(function() pc.CheatClass = cls end)
    pcall(function() pc.CheatManager = cm end)
    pcall(function() cm:ReceiveInitCheatManager() end)
    E.cheatClassCache = nil
    local _, n = WriteCheats()
    Msg(string.format("Using %s - %d cheats", ((path:match("([^/%.]+)$") or path):gsub("_C$", "")), n or 0))
end
handlers.ed_devmenu = function(path)
    local pc = PlayerPC(); if not Valid(pc) then Msg("No player right now.", true); return end
    local wbl = StaticFindObject("/Script/UMG.Default__WidgetBlueprintLibrary")
    local w = devWidgets[path]
    local showing = false
    if Valid(w) then pcall(function() showing = w:IsInViewport() end) end
    if showing then
        pcall(function() w:RemoveFromParent() end)
        devWidgets[path] = nil
        pcall(function() wbl:SetInputMode_GameOnly(pc) end)
        pcall(function() pc.bShowMouseCursor = false end)
        Msg("Closed " .. ((path:match("([^/%.]+)$") or path):gsub("_C$", "")))
    else
        local cls = ClassFor(path)
        if not cls then Msg("Couldn't load " .. path, true); return end
        local ok, nw = pcall(function() return wbl:Create(World(), cls, pc) end)
        if not (ok and Valid(nw)) then Msg("The game wouldn't open that menu.", true); return end
        pcall(function() nw:AddToViewport(1000) end)
        pcall(function() wbl:SetInputMode_GameAndUIEx(pc, nw, 0, false) end)
        pcall(function() pc.bShowMouseCursor = true end)
        devWidgets[path] = nw
        Msg("Opened " .. ((path:match("([^/%.]+)$") or path):gsub("_C$", "")) .. " (click into the game to use it)")
    end
    WriteCheats()
end

-- Objects set to "Static" can't be moved while the game runs; switch them (and what's attached) to Movable first.
local madeMovable = {}
local function MakeMovable(a)
    local key = tostring(a:GetAddress())
    if madeMovable[key] then return end
    madeMovable[key] = true
    pcall(function()
        local root = a:K2_GetRootComponent()
        if not Valid(root) then return end
        local okM, m = pcall(function() return root.Mobility end)
        if okM and tonumber(m) == 2 then return end
        root:SetMobility(2)   -- EComponentMobility::Movable
    end)
end
local warnedStuck = {}
local function CheckMoved(a, x, y, z)
    local ok, l = pcall(function() return a:K2_GetActorLocation() end)
    if not ok or not l then return end
    if math.abs(l.X - x) + math.abs(l.Y - y) + math.abs(l.Z - z) > 1 then
        local key = tostring(a:GetAddress())
        if not warnedStuck[key] then
            warnedStuck[key] = true
            Msg("The game won't let " .. Str(a:GetFName()) .. " move (it's locked in place by the game).", true)
        end
    end
end

handlers.ed_xform = function(arg)   -- path \t loc \t rot \t scale
    local path, ls, rs, ss = arg:match("^([^\t]+)\t([^\t]*)\t([^\t]*)\t([^\t]*)$")
    local a = path and FindActor(path); if not a then Msg("That actor no longer exists.", true); return end
    local x, y, z = Num3(ls); local p, yw, r = Num3(rs); local sx, sy, sz = Num3(ss)
    MakeMovable(a)
    if x and p then
        local hit = {}
        local ok = pcall(function() a:K2_SetActorLocationAndRotation(Vec(x, y, z), Rot(p, yw, r), false, hit, true) end)
        if not ok then pcall(function() a:K2_TeleportTo(Vec(x, y, z), Rot(p, yw, r)) end) end
        CheckMoved(a, x, y, z)
    end
    if sx then pcall(function() a:SetActorScale3D(Vec(sx, sy, sz)) end) end
end
-- Rotates around a world axis through the object's centre: path \t axis(0-2) \t degrees \t startLoc \t startRot \t centre
handlers.ed_rot = function(arg)
    local path, ax, deg, ls, rs, cs = arg:match("^([^\t]+)\t(%d)\t([%-%d%.eE]+)\t([^\t]+)\t([^\t]+)\t([^\t]+)$")
    local a = path and FindActor(path); if not a then Msg("That actor no longer exists.", true); return end
    local x, y, z = Num3(ls); local p, yw, r = Num3(rs); local cx, cy, cz = Num3(cs)
    if not (x and p and cx) then return end
    deg = tonumber(deg) or 0
    local axis = ({ [0] = Vec(1, 0, 0), [1] = Vec(0, 1, 0), [2] = Vec(0, 0, 1) })[tonumber(ax)]
    local kml = UEHelpers.GetKismetMathLibrary()
    local dr = kml:RotatorFromAxisAndAngle(axis, deg)
    local nr = kml:ComposeRotators(Rot(p, yw, r), dr)
    local off = kml:RotateAngleAxis(Vec(x - cx, y - cy, z - cz), deg, axis)
    local nl = Vec(cx + off.X, cy + off.Y, cz + off.Z)
    local nrot = Rot(nr.Pitch, nr.Yaw, nr.Roll)
    MakeMovable(a)
    local hit = {}
    if not pcall(function() a:K2_SetActorLocationAndRotation(nl, nrot, false, hit, true) end) then a:K2_TeleportTo(nl, nrot) end
end

handlers.ed_hide = function(arg)
    local path, v = arg:match("^([^\t]+)\t(%d)$")
    local a = path and FindActor(path); if not a then Msg("That actor no longer exists.", true); return end
    a:SetActorHiddenInGame(v == "1")
    Msg(v == "1" and "Hidden" or "Shown")
end
handlers.ed_destroy = function(arg)
    local a = FindActor(arg); if not a then Msg("That actor no longer exists.", true); return end
    local n = Str(a:GetFName())
    local isPawn = false
    pcall(function() isPawn = a:IsA(StaticFindObject("/Script/Engine.Pawn")) end)
    local _, myPawn = PlayerPC()
    if isPawn or not E.SoftDelete or (Valid(myPawn) and myPawn:GetAddress() == a:GetAddress()) then
        a:K2_DestroyActor()
        Msg("Deleted " .. n .. (isPawn and " (characters can't be brought back with Undo)" or ""))
    else
        E.SoftDelete(arg, a)
        Msg("Deleted " .. n)
    end
    if selectedPath == arg then selected, selectedPath = nil, "" end
    actorCache[arg] = nil
    E.WriteProps()
end
handlers.ed_goto = function(arg)
    local a = FindActor(arg); if not a then return end
    local _, pawn = PlayerPC(); if not Valid(pawn) then return end
    local l = a:K2_GetActorLocation()
    pawn:K2_TeleportTo(Vec(l.X, l.Y, l.Z + 200), Rot(0, 0, 0))
    Msg("Teleported to " .. Str(a:GetFName()))
end
handlers.ed_bring = function(arg)
    local a = FindActor(arg); if not a then return end
    local vpc = ViewPC()
    local pcm = vpc.PlayerCameraManager
    local l, r = pcm:GetCameraLocation(), pcm:GetCameraRotation()
    local yaw = math.rad(r.Yaw)
    local hit = {}
    local dest = Vec(l.X + math.cos(yaw) * 400, l.Y + math.sin(yaw) * 400, l.Z)
    if not pcall(function() a:K2_SetActorLocation(dest, false, hit, true) end) then a:K2_TeleportTo(dest, a:K2_GetActorRotation()) end
    Msg("Moved " .. Str(a:GetFName()) .. " in front of you")
end
handlers.ed_set = function(arg)     -- path \t prop \t kind \t value
    local path, prop, kind, val = arg:match("^([^\t]+)\t([^\t]+)\t([^\t]+)\t(.*)$")
    local a = path and FindActor(path); if not a then Msg("That actor no longer exists.", true); return end
    local v
    if kind == "bool" then v = (val == "1")
    elseif kind == "float" then v = tonumber(val)
    elseif kind == "int" then v = math.floor(tonumber(val) or 0)
    elseif kind == "str" then v = val
    else Msg(prop .. " can't be edited here.", true); return end
    if v == nil then return end
    local ok, err = pcall(function() a[prop] = v end)
    if not ok then Msg("Couldn't set " .. prop .. ": " .. tostring(err), true) end
end

-- Runs a command (called from the bridge's command loop). Returns true if it was an editor command.

-- ---------------------------------------------------------------- models (add static meshes to the world)
local spawned = {}      -- { path, name } of models added this session (newest last)

-- Every model (static mesh) the game has loaded right now.
function E.ListModels(filter)
    filter = (filter or ""):lower()
    local rows, seen = {}, {}
    for _, m in ipairs(FindAllOf("StaticMesh") or {}) do
        if Valid(m) then
            local path = ObjectPath(m)
            if path ~= "" and not seen[path] and not path:find(":", 1, true) and not path:find("^/Engine/") then
                seen[path] = true
                local name = path:match("([^/%.]+)$") or path
                if filter == "" or path:lower():find(filter, 1, true) then table.insert(rows, { path = path, name = name }) end
            end
        end
    end
    table.sort(rows, function(a, b) return a.name:lower() < b.name:lower() end)
    local lines = { "filter=" .. Clean(filter), "total=" .. #rows }
    for i = 1, math.min(#rows, 4000) do table.insert(lines, "m=" .. rows[i].path .. "\t" .. Clean(rows[i].name)) end
    WriteFile(Dir .. "models.txt", table.concat(lines, "\n") .. "\n")
    Msg(string.format("Found %d loaded models%s", #rows, filter ~= "" and (" matching \"" .. filter .. "\"") or ""))
end

local function FindMesh(path, noLoad)
    if not path or path == "" then return nil end
    if not path:find("%.") then path = path .. "." .. (path:match("([^/]+)$") or "") end   -- /Game/X/SM_A -> /Game/X/SM_A.SM_A
    local ok, m = pcall(function() return StaticFindObject(path) end)
    if not (ok and Valid(m)) and not noLoad then
        pcall(function() LoadAsset(path) end)                -- not loaded yet: load it from the game's files
        ok, m = pcall(function() return StaticFindObject(path) end)
    end
    if not (ok and Valid(m)) then return nil, "Couldn't find or load " .. path end
    local okA, isMesh = pcall(function() return m:IsA("/Script/Engine.StaticMesh") end)
    if not (okA and isMesh) then return nil, (path:match("([^/%.]+)$") or path) .. " isn't a model (static mesh)" end
    return m
end

-- Where a point on screen (default: the middle) hits the world, or a few metres ahead if it hits nothing.
local function AimPoint(nx, ny)
    local vpc = ViewPC()
    local pcm = vpc.PlayerCameraManager
    local start = pcm:GetCameraLocation()
    local fwd = UEHelpers.GetKismetMathLibrary():GetForwardVector(pcm:GetCameraRotation())
    if nx and ny then
        pcall(function()
            local size = {}
            vpc:GetViewportSize(size, size)
            local w, h = tonumber(size.SizeX) or 0, tonumber(size.SizeY) or 0
            local loc, dir = {}, {}
            if w > 0 and h > 0 and vpc:DeprojectScreenPositionToWorld(nx * w, ny * h, loc, dir) and loc.X and dir.X then
                start, fwd = { X = loc.X, Y = loc.Y, Z = loc.Z }, { X = dir.X, Y = dir.Y, Z = dir.Z }
            end
        end)
    end
    local _, pawn = PlayerPC()
    local ctx = Valid(pawn) and pawn or ViewPC()
    local color = { R = 0, G = 0, B = 0, A = 0 }
    local s, e = { X = start.X, Y = start.Y, Z = start.Z }, Vadd(start, fwd, 5000)
    local hit = {}
    local ok, wasHit = pcall(function()
        return UEHelpers.GetKismetSystemLibrary():LineTraceSingle(ctx, s, e, 0, false, {}, 0, hit, true, color, color, 0.0)
    end)
    if ok and wasHit then
        local p = hit.ImpactPoint or hit.Location
        if p and p.X then return { X = p.X, Y = p.Y, Z = p.Z }, true, pcm:GetCameraRotation() end
    end
    return Vadd(start, fwd, 500), false, pcm:GetCameraRotation()
end

-- Spawns an actor of a class, using UE4SS's SpawnActor or, failing that, the engine's deferred spawn.
local function SpawnOf(classPath, loc, rot)
    local world = World()
    local cls = StaticFindObject(classPath)
    if not Valid(world) or not Valid(cls) then return nil end
    local ok, a = pcall(function() return world:SpawnActor(cls, loc, rot) end)
    if not (ok and Valid(a)) then
        local gs = nil
        pcall(function() gs = UEHelpers.GetGameplayStatics() end)
        if not Valid(gs) then gs = StaticFindObject("/Script/Engine.Default__GameplayStatics") end
        -- FRotator -> FQuat (same maths as the engine)
        local h = math.pi / 360
        local sp, cp = math.sin(rot.Pitch * h), math.cos(rot.Pitch * h)
        local sy, cy = math.sin(rot.Yaw * h), math.cos(rot.Yaw * h)
        local sr, cr = math.sin(rot.Roll * h), math.cos(rot.Roll * h)
        local q = { X = cr * sp * sy - sr * cp * cy, Y = -cr * sp * cy - sr * cp * sy, Z = cr * cp * sy - sr * sp * cy, W = cr * cp * cy + sr * sp * sy }
        local tf = { Translation = loc, Rotation = q, Scale3D = { X = 1, Y = 1, Z = 1 } }
        ok, a = pcall(function() return gs:BeginDeferredActorSpawnFromClass(world, cls, tf, 1, nil) end)
        if not (ok and Valid(a)) then return nil end
        pcall(function() gs:FinishSpawningActor(a, tf) end)
    end
    return a
end


-- Games where the player can only jump onto marked surfaces: give placed models the same marking, so you can
-- jump onto them and walk on them like the game's own objects. Stray marks them with "rails" (a spline with a width
-- along the top of a prop); bmin/bmax are the model's box in its own space.
E.walkable = true
function E.AddWalkable(actor, bmin, bmax)
    if not E.walkable then return end
    if not (Valid(actor) and bmin and bmax) then return end
    local cls = StaticFindObject("/Script/Hk_project.SplineRailComponent")
    if not Valid(cls) then return end
    local sx, sy = bmax[1] - bmin[1], bmax[2] - bmin[2]
    if sx < 15 and sy < 15 then return end                       -- too small to stand on
    local cx, cy, top = (bmin[1] + bmax[1]) / 2, (bmin[2] + bmax[2]) / 2, bmax[3]
    -- laid out like the game's own (TV stands, jump boxes): the rail starts at one end of the top and runs along
    -- its length (the rail's own X), two points, width a bit less than the top's depth
    local alongX = sx >= sy
    local len, depth = alongX and sx or sy, alongX and sy or sx
    len = math.max(10, len - 10)
    local start = alongX and { X = cx - len / 2, Y = cy, Z = top + 1 } or { X = cx, Y = cy - len / 2, Z = top + 1 }
    local h = alongX and 0 or math.sin(math.pi / 4)            -- yaw 0 or 90 degrees
    local xf = { Rotation = { X = 0, Y = 0, Z = h, W = alongX and 1 or math.cos(math.pi / 4) }, Translation = start, Scale3D = { X = 1, Y = 1, Z = 1 } }
    -- added "deferred": its shape is set before the game sets it up (it builds its collision and ground
    -- connections from the shape when it starts)
    local ok, rail = pcall(function() return actor:AddComponentByClass(cls, false, xf, true) end)
    if not (ok and Valid(rail)) then return end
    pcall(function() rail:SetLocationAtSplinePoint(0, { X = 0, Y = 0, Z = 0 }, 0, false) end)
    pcall(function() rail:SetLocationAtSplinePoint(1, { X = len, Y = 0, Z = 0 }, 0, true) end)
    pcall(function() rail.Width = math.max(15, math.min(60, depth * 0.6)) end)
    pcall(function() actor:FinishAddComponent(rail, false, xf) end)
    pcall(function() rail:SetEnabled(true) end)
    return rail
end
function E.MeshBox(mesh)
    local ok, b = pcall(function() return mesh:GetBounds() end)
    if not (ok and b and b.Origin and b.BoxExtent) then return nil end
    local o, e = b.Origin, b.BoxExtent
    return { o.X - e.X, o.Y - e.Y, o.Z - e.Z }, { o.X + e.X, o.Y + e.Y, o.Z + e.Z }
end

local function SpawnMeshActor(mesh, loc, rot, scale)
    local a = SpawnOf("/Script/Engine.StaticMeshActor", loc, rot)
    if not a then return nil end
    local comp = a.StaticMeshComponent
    if Valid(comp) then
        pcall(function() comp:SetMobility(2) end)
        comp:SetStaticMesh(mesh)
    end
    local hit = {}
    pcall(function() a:K2_SetActorLocationAndRotation(loc, rot, false, hit, true) end)
    if scale then pcall(function() a:SetActorScale3D(scale) end) end
    pcall(function() local bn, bx = E.MeshBox(mesh); E.AddWalkable(a, bn, bx) end)
    return a
end

local Lay   -- saved layout (defined further down)
local function Added(a, mesh)
    local path = ObjectPath(a)
    if Lay then pcall(Lay.AddedNew, a, mesh) end
    if E.UndoAdded then pcall(E.UndoAdded, path) end
    local name = (ObjectPath(mesh):match("([^/%.]+)$")) or Str(a:GetFName())
    table.insert(spawned, { path = path, name = name })
    if #spawned > 200 then table.remove(spawned, 1) end
    E.Select(path, a)
    Msg("Added " .. name)
end

-- arg: model path [\t screenX,screenY]. Placed where you clicked (or the middle of the screen), sitting on the surface.
handlers.ed_spawn = function(arg)
    local path, at = arg:match("^([^\t]+)\t?(.*)$")
    local mesh, err = FindMesh(path)
    if not mesh then Msg(err, true); return end
    local nx, ny = (at or ""):match("^([%-%d%.]+),([%-%d%.]+)$")
    local loc, onSurface, crot = AimPoint(tonumber(nx), tonumber(ny))
    if onSurface then   -- sit it on the surface rather than half inside it
        pcall(function()
            local b = mesh:GetBounds()
            if b and b.BoxExtent and b.Origin then loc.Z = loc.Z + b.BoxExtent.Z - b.Origin.Z end
        end)
    end
    local a = SpawnMeshActor(mesh, loc, Rot(0, (crot and crot.Yaw or 0) + 180, 0))
    if not a then Msg("The game wouldn't add that model.", true); return end
    Added(a, mesh)
end

-- arg: class path [\t screenX,screenY]: an actor by class (the editor's Place Actors panel: lights, cameras, text)
handlers.ed_spawnclass = function(arg)
    local cls, at = arg:match("^([^\t]+)\t?(.*)$")
    if not cls then return end
    local nx, ny = (at or ""):match("^([%-%d%.]+),([%-%d%.]+)$")
    local loc, onSurface, crot = AimPoint(tonumber(nx), tonumber(ny))
    if onSurface then loc.Z = loc.Z + 60 end                     -- (a light or camera a little above the surface)
    local a = SpawnOf(cls, loc, Rot(0, (crot and crot.Yaw or 0) + 180, 0))
    if not a then Msg("The game wouldn't add " .. (cls:match("([^%.]+)$") or cls) .. " (it may not include it).", true); return end
    pcall(function() a.RootComponent:SetMobility(2) end)      -- movable: shows straight away and can be moved
    pcall(function() a.LightComponent:SetMobility(2) end)
    if cls:find("TextRender", 1, true) then pcall(function() a.TextRender:K2_SetText(FText and FText("Text") or "Text") end) end
    local path = ObjectPath(a)
    if E.UndoAdded then pcall(E.UndoAdded, path) end
    E.Select(path, a)
    Msg("Added " .. (cls:match("([^%.]+)$") or "actor") .. " (not saved with the level's layout)")
end
-- Unreal's F key: fly the editing camera to the selected object
handlers.ed_focus = function(arg)
    local a = FindActor(arg); if not a then return end
    local origin, extent = {}, {}
    local ok = pcall(function() a:GetActorBounds(false, origin, extent, false) end)
    if not ok then origin, extent = {}, {}; pcall(function() a:GetActorBounds(false, origin, extent) end) end
    local cx, cy, cz = origin.X, origin.Y, origin.Z
    if type(cx) ~= "number" then local l = a:K2_GetActorLocation(); cx, cy, cz = l.X, l.Y, l.Z end
    local r = 100
    if type(extent.X) == "number" then r = math.sqrt(extent.X ^ 2 + extent.Y ^ 2 + extent.Z ^ 2) end
    local dist = math.max(250, math.min(r * 2.6, 20000))
    if not (Freecam and Freecam.FocusOn and Freecam.FocusOn(cx, cy, cz, dist)) then
        Msg("Turn on the editing camera (Freecam while editing) to fly to things with F.", true)
    end
end

-- ---------------------------------------------------------------- imported models (.uemodel, made by the launcher)
-- A model from another game (or anywhere) is built here out of procedural mesh parts, one per bone, coloured with
-- vertex colours. Animations play by moving the parts. Needs the game to include Unreal's ProceduralMeshComponent
-- (most do) and Unreal Engine 4.25 or newer (AddComponentByClass).
local Models = { cache = {}, live = {}, order = {} }
local function ParseModel(file)
    if Models.cache[file] then return Models.cache[file] end
    local f = io.open(file, "r"); if not f then return nil, "Couldn't open that model file." end
    local m = { parts = {}, anims = {}, name = "model", minZ = 1e9, mats = {}, dir = file:match("^(.*[\\/])") or "" }
    local part, anim
    for line in f:lines() do
        local c = line:sub(1, 2)
        if c == "v " then
            local x, y, z, nx, ny, nz, r, g, b, rest = line:match("^v (%S+) (%S+) (%S+) (%S+) (%S+) (%S+) (%d+) (%d+) (%d+) ?(.*)$")
            local n = #part.verts + 1
            local u, vv, tx, ty, tz, fl = (rest or ""):match("^(%S+) (%S+) ?(%S*) ?(%S*) ?(%S*) ?(%S*)")
            part.uvs[n] = { X = tonumber(u) or 0, Y = tonumber(vv) or 0 }
            if tonumber(tx) then part.tangents[n] = { TangentX = { X = tonumber(tx), Y = tonumber(ty), Z = tonumber(tz) }, bFlipTangentY = fl == "1" } end
            x, y, z = tonumber(x), tonumber(y), tonumber(z)
            part.verts[n] = { X = x, Y = y, Z = z }
            part.normals[n] = { X = tonumber(nx), Y = tonumber(ny), Z = tonumber(nz) }
            part.colors[n] = { R = tonumber(r), G = tonumber(g), B = tonumber(b), A = 255 }
            part.lcolors[n] = { R = tonumber(r) / 255, G = tonumber(g) / 255, B = tonumber(b) / 255, A = 1.0 }
            if z < m.minZ then m.minZ = z end
            if not m.bmin then m.bmin = { x, y, z }; m.bmax = { x, y, z } end
            local bn, bx = m.bmin, m.bmax
            if x < bn[1] then bn[1] = x elseif x > bx[1] then bx[1] = x end
            if y < bn[2] then bn[2] = y elseif y > bx[2] then bx[2] = y end
            if z < bn[3] then bn[3] = z elseif z > bx[3] then bx[3] = z end
        elseif c == "t " then
            local a, b, cc = line:match("^t (%d+) (%d+) (%d+)")
            local tr = part.tris; local n = #tr; local o = part.base or 0
            tr[n + 1] = tonumber(a) + o; tr[n + 2] = tonumber(b) + o; tr[n + 3] = tonumber(cc) + o
            m.tris = (m.tris or 0) + 1
        elseif m.skinned and (c == "w " or c == "b " or c == "k " or line == "w") then
            -- (a skinned model's bone weights and poses are read by the launcher's helper inside the game, not here)
        elseif c == "w " then         -- skin weights of the vertex just read: skin bone, weight, ...
            local t, n = {}, #part.verts
            for x in line:gmatch("%S+") do if x ~= "w" then t[#t + 1] = tonumber(x) end end
            part.skin = part.skin or {}; part.skinIdx = part.skinIdx or {}
            part.skin[n] = t; part.skinIdx[#part.skinIdx + 1] = n
        elseif c == "b " then         -- a skin bone's pose for this frame: quaternion x y z w, move x y z
            local qx, qy, qz, qw, tx, ty, tz = line:match("^b (%S+) (%S+) (%S+) (%S+) (%S+) (%S+) (%S+)")
            anim.b[#anim.b + 1] = { tonumber(qx), tonumber(qy), tonumber(qz), tonumber(qw), tonumber(tx), tonumber(ty), tonumber(tz) }
        elseif c == "k " then
            local x, y, z, pi, ya, ro = line:match("^k (%S+) (%S+) (%S+) (%S+) (%S+) (%S+)")
            anim.k[#anim.k + 1] = { tonumber(x), tonumber(y), tonumber(z), tonumber(pi), tonumber(ya), tonumber(ro) }
        elseif c == "cv" then         -- the light collision copy of a big model (model space)
            local x, y, z = line:match("^cv (%S+) (%S+) (%S+)")
            local cv = m.coll.verts; cv[#cv + 1] = { X = tonumber(x), Y = tonumber(y), Z = tonumber(z) }
        elseif c == "ct" then
            local a, b, cc = line:match("^ct (%d+) (%d+) (%d+)")
            local ct = m.coll.tris; local n = #ct
            ct[n + 1] = tonumber(a); ct[n + 2] = tonumber(b); ct[n + 3] = tonumber(cc)
        elseif line:sub(1, 5) == "coll\t" then m.coll = { verts = {}, tris = {} }
        elseif line:sub(1, 5) == "part\t" then
            local pname, pmat = line:match("^part\t([^\t]*)\t%d+\t%d+\t?(%d*)")
            if m.skinned == nil then m.skinned = (m.nskin or 0) > 0 and (m.nanimnames or 0) > 0 end
            if m.skinned then       -- one piece per material (the skin is bent as a whole, not moved piece by piece)
                local key = tonumber(pmat) or -1
                m.byMat = m.byMat or {}
                part = m.byMat[key]
                if not part then
                    part = { name = "skin" .. key, mat = tonumber(pmat), verts = {}, normals = {}, colors = {}, lcolors = {}, uvs = {}, tangents = {}, tris = {} }
                    m.byMat[key] = part; table.insert(m.parts, part)
                end
                part.base = #part.verts
            else
                part = { name = pname or "part", mat = tonumber(pmat), verts = {}, normals = {}, colors = {}, lcolors = {}, uvs = {}, tangents = {}, tris = {} }
                table.insert(m.parts, part)
            end
        elseif line:sub(1, 3) == "fx\t" then
            local f = SplitTabs(line)        -- fx, kind, asset, x,y,z, pitch,yaw,roll, sx,sy,sz
            local function v3(t) local a, b, c = (t or ""):match("([^,]+),([^,]+),([^,]+)"); return tonumber(a) or 0, tonumber(b) or 0, tonumber(c) or 0 end
            local x, y, z = v3(f[4]); local pi, ya, ro = v3(f[5]); local sx, sy, sz = v3(f[6])
            m.fx = m.fx or {}
            table.insert(m.fx, { kind = f[2], path = f[3], loc = { X = x, Y = y, Z = z }, rot = { Pitch = pi, Yaw = ya, Roll = ro }, scale = { X = sx, Y = sy, Z = sz } })
        elseif line:sub(1, 5) == "hull\t" then
            local f = SplitTabs(line)        -- hull, count, x,y,z ... (a collision shape: the convex hull of these points)
            local h = {}
            for k = 3, #f do local x, y, z = f[k]:match("([^,]+),([^,]+),([^,]+)"); if tonumber(z) then h[#h + 1] = { X = tonumber(x), Y = tonumber(y), Z = tonumber(z) } end end
            if #h >= 4 then m.hulls = m.hulls or {}; table.insert(m.hulls, h) end
        elseif line:sub(1, 5) == "real\t" then
            local f = SplitTabs(line)        -- real, kind, object path, x,y,z, pitch,yaw,roll, sx,sy,sz
            local function v3(t, d) local a, b, c = (t or ""):match("([^,]+),([^,]+),([^,]+)"); return tonumber(a) or d, tonumber(b) or d, tonumber(c) or d end
            local x, y, z = v3(f[4], 0); local pi, ya, ro = v3(f[5], 0); local sx, sy, sz = v3(f[6], 1)
            m.real = m.real or {}
            table.insert(m.real, { kind = f[2], path = f[3], loc = { X = x, Y = y, Z = z }, rot = { Pitch = pi, Yaw = ya, Roll = ro }, scale = { X = sx, Y = sy, Z = sz } })
        elseif line:sub(1, 5) == "anim\t" then
            local name, fps, frames, apath = line:match("^anim\t([^\t]*)\t(%S+)\t(%d+)\t?([^\t]*)")
            anim = { name = name, fps = tonumber(fps) or 30, frames = tonumber(frames) or 0, k = {}, b = {}, path = (apath ~= "" and apath) or nil }
            table.insert(m.anims, anim)
        elseif line:sub(1, 4) == "mat\t" then
            local f = SplitTabs(line)          -- mat, i, colour, rgb, normal, name, orm, blend, twoSided, clip, avgAlpha, alpha
            local i = tonumber(f[2])
            if i then
                local function file(x) return (x and x ~= "" and x ~= "-") and (m.dir .. x) or false end
                m.mats[i] = file(f[3])
                m.normals = m.normals or {}; m.normals[i] = file(f[5])
                m.masks = m.masks or {}; m.masks[i] = file(f[7])
                m.looks = m.looks or {}; m.looks[i] = ParseLook(f, 8, file(f[12]))
                m.origs = m.origs or {}; m.origs[i] = (f[13] and f[13] ~= "" and f[13] ~= "-") and f[13] or false
                -- glow (self-illumination): its picture, its colour x strength, and the picture's average colour
                local er, eg, eb = (f[15] or ""):match("([^,]+),([^,]+),([^,]+)")
                if tonumber(er) and (tonumber(er) + tonumber(eg) + tonumber(eb)) > 0 then
                    m.emis = m.emis or {}
                    local ar, ag, ab = (f[16] or ""):match("([^,]+),([^,]+),([^,]+)")
                    m.emis[i] = { file = file(f[14]), r = tonumber(er), g = tonumber(eg), b = tonumber(eb), ar = tonumber(ar) or 1, ag = tonumber(ag) or 1, ab = tonumber(ab) or 1 }
                end
            end
        elseif line:sub(1, 10) == "skinbones\t" then m.nskin = tonumber(line:sub(11)) or 0
        elseif line:sub(1, 9) == "animname\t" then m.nanimnames = (m.nanimnames or 0) + 1
        elseif line:sub(1, 5) == "name\t" then m.name = line:sub(6) end
    end
    f:close()
    -- materials picked from the library for some slots (written by the launcher next to the model)
    local sf = io.open(file .. ".mats", "r")
    if sf then
        for line in sf:lines() do
            local f = SplitTabs(line)          -- slot, i, name, colour, normal, orm, blend, twoSided, clip, avgAlpha, alpha
            local i, c = tonumber(f[2]), f[4]
            local cf = i and c and io.open(c, "rb")
            if cf then
                cf:close()
                local function file(x) return (x and x ~= "" and x ~= "-") and x or false end
                m.mats[i] = c
                m.normals = m.normals or {}; m.normals[i] = file(f[5])
                m.masks = m.masks or {}; m.masks[i] = file(f[6])
                m.looks = m.looks or {}; m.looks[i] = ParseLook(f, 7, file(f[11]))
                m.origs = m.origs or {}
                if #f >= 12 then m.origs[i] = file(f[12]) end   -- (older choice files don't say: keep the model's own)
                if m.emis then m.emis[i] = nil end            -- (a different material: not the original's glow)
            end
        end
        sf:close()
    end
    if #m.parts == 0 then return nil, "That model file is empty." end
    m.byMat = nil
    if m.minZ > 1e8 then m.minZ = 0 end
    Models.cache[file] = m
    return m
end
-- A material that shows vertex colours. The engine keeps a few of these loaded (its debug view modes use them);
-- which ones exist depends on the game and engine version, so try them in order and remember the one that works.
local vcMat, vcMatTried = nil, false
local function VertexColourMaterial()
    if vcMatTried then return vcMat end
    vcMatTried = true
    local eng = nil
    pcall(function() eng = FindFirstOf("GameEngine") end)
    if not Valid(eng) then pcall(function() eng = FindFirstOf("Engine") end) end
    if Valid(eng) then
        for _, prop in ipairs({ "VertexColorViewModeMaterial_ColorOnly", "VertexColorMaterial", "VertexColorViewModeMaterial_AlphaAsColor" }) do
            local ok, m = pcall(function() return eng[prop] end)
            if ok and Valid(m) and prop ~= "VertexColorViewModeMaterial_AlphaAsColor" then vcMat = m; break end
        end
    end
    if not vcMat then
        for _, p in ipairs({ "/Engine/EngineDebugMaterials/VertexColorViewModeMaterial_ColorOnly.VertexColorViewModeMaterial_ColorOnly",
                             "/Engine/EngineDebugMaterials/VertexColorMaterial.VertexColorMaterial" }) do
            local m = StaticFindObject(p)
            if not Valid(m) then pcall(function() LoadAsset(p) end); m = StaticFindObject(p) end
            if Valid(m) then vcMat = m; break end
        end
    end
    print("[LauncherBridge] [Editor] imported-model material: " .. (vcMat and ObjectPath(vcMat) or "none found (models will be grey)") .. "\n")
    return vcMat
end
-- A material from the game itself that has a colour-texture slot, so imported models can show their real texture:
-- try the game's material instances, make a dynamic copy, and see if it has a texture parameter with a colour name.
local texBase, texParam, texTried = nil, nil, false
local TEX_PARAMS = { "BaseColor", "Base Color", "BaseColorTexture", "BaseColorMap", "BaseColor_Texture", "Base_Color", "Albedo", "AlbedoTexture",
    "Diffuse", "DiffuseTexture", "DiffuseMap", "Color", "ColorTexture", "Colour", "Texture", "Tex", "MainTexture", "MainTex", "BC", "CO", "D", "T_BaseColor" }
local BAD_NAMES = { "decal", "ui", "hud", "widget", "landscape", "terrain", "foliage", "grass", "water", "glass", "hair", "eye", "skin", "particle",
    "fx", "vfx", "sky", "cloud", "fog", "translucent", "emissive", "light", "post", "outline", "mask", "blend", "trim" }
local OTHER_TEX = { "Normal", "NormalMap", "Normal Map", "NormalTexture", "N", "NM", "ORM", "ARM", "MRA", "RMA", "Roughness", "RoughnessMap",
    "Metallic", "Metalness", "MetallicMap", "Mask", "Masks", "MaskTexture", "Emissive", "EmissiveMap", "Detail", "DetailNormal", "AO", "Height", "LP" }
local NEUTRAL_SCALARS = { Metallic = 0, Metalness = 0, Metal = 0, ["Metallic Amount"] = 0, Roughness = 0.75, ["Roughness Amount"] = 0.75,
    RoughnessMin = 0, RoughnessMax = 1, Specular = 0.5, RoughnessMultiplier = 1, ["Roughness Multiplier"] = 1, MetallicMultiplier = 1,
    ["Metallic Multiplier"] = 1, AOStrength = 1, ["AO Strength"] = 1, AmbientOcclusionStrength = 1, SpecularMultiplier = 1, EmissiveStrength = 0, Emissive = 0, ["Emissive Intensity"] = 0, EmissiveIntensity = 0,
    NormalStrength = 1, ["Normal Strength"] = 1, NormalIntensity = 1, Brightness = 1, Contrast = 1, Saturation = 1, Opacity = 1, Tiling = 1, UVTiling = 1, ["UV Tiling"] = 1 }
local NEUTRAL_COLOURS = { "Tint", "Color Tint", "ColorTint", "BaseColorTint", "Base Color Tint", "BaseColor Tint", "Albedo Tint", "AlbedoTint", "DiffuseTint", "Colour", "ColorMultiplier", "Color Multiplier" }
-- the names an imported model's material copy gets set on every copy (whether the material has them or not)
E.NEUTRAL_NAMES = { EmissiveColor = true, ["Emissive Color"] = true, EmissiveColour = true, DirtMaskSelect = true }
for n in pairs(NEUTRAL_SCALARS) do E.NEUTRAL_NAMES[n] = true end
for _, n in ipairs(NEUTRAL_COLOURS) do E.NEUTRAL_NAMES[n] = true end
function SplitTabs(line)
    local f = {}
    for x in (line:gsub("\r$", "") .. "\t"):gmatch("([^\t]*)\t") do f[#f + 1] = x end
    return f
end
-- how the original material draws: blend (0 opaque 1 masked 2 translucent 3 additive 4 modulate), two-sided, cut-out threshold
local BLENDS = { opaque = 0, masked = 1, translucent = 2, additive = 3, modulate = 4 }
function ParseLook(f, k, alphaFile)
    return { blend = BLENDS[f[k] or ""] or 0, two = f[k + 1] == "1", clip = tonumber(f[k + 2]) or 0.333, alpha = tonumber(f[k + 3]) or 1, alphaFile = alphaFile }
end
local BLEND_NAMES = { [0] = "opaque", "masked", "translucent", "additive", "modulate" }
-- the other texture slots a borrowed material may have, and what to put in them
local NORMAL_SLOTS = { "Normal", "NormalMap", "Normal Map", "NormalTexture", "Normal_Texture", "NormalTex", "Normals", "N", "NM", "NRM" }
local NEUTRAL_SLOTS = { ORM = "orm", ARM = "orm", ["Occlusion Roughness Metallic"] = "orm", OcclusionRoughnessMetallic = "orm", MRA = "mra", RMA = "rma",
    MRO = "mra", MRAO = "mra", RMO = "rma",
    Roughness = "rough", RoughnessMap = "rough", RoughnessTexture = "rough", Metallic = "metal", Metalness = "metal", MetallicMap = "metal",
    MetallicTexture = "metal", AO = "ao", AmbientOcclusion = "ao", Occlusion = "ao", Emissive = "black", EmissiveMap = "black",
    EmissiveTexture = "black", Specular = "spec" }
-- which of those the model's own surface masks (from the original material) can fill
local MASK_KINDS = { orm = "", mra = "_mra", rma = "_rma", rough = "_rough", metal = "_metal", ao = "_ao" }
local UNKNOWN_SLOTS = { "Mask", "Masks", "MaskTexture", "Packed", "PackedTexture", "LP", "SRMH", "Detail", "DetailNormal" }
local OPACITY_SLOTS = { "Opacity", "OpacityMask", "Opacity Mask", "OpacityTexture", "Opacity_Texture", "OpacityMap", "Alpha", "AlphaMask", "Alpha Mask", "AlphaTexture" }
local texPick, micList = {}, nil
-- ---- what each texture slot of a game material really is: read from the material's own parameter list and the
-- texture in each slot (a normal map is stored with normal-map compression, a colour picture as sRGB, masks as linear)
local function Ends(s, ...) for _, w in ipairs({ ... }) do if #s > #w and s:sub(-#w) == w then return true end end return false end
local function TexKind(param, tex)
    local pn, tn = param:lower(), ""
    pcall(function() tn = tex:GetFName():ToString():lower() end)
    local comp, srgb = nil, nil
    pcall(function() comp = tonumber(tex.CompressionSettings) end)
    pcall(function() srgb = tex.SRGB end)
    local both = pn .. " " .. tn
    local function has(...) for _, w in ipairs({ ... }) do if both:find(w, 1, true) then return true end end return false end
    if comp == 1 or has("normal") or Ends(tn, "_n", "_nm", "_nrm", "_norm", "_nor") or pn == "n" or pn == "nm" then return "normal" end
    if has("opacity", "alpha") then return "opacity" end
    if has("emissive", "emission", "glow") or Ends(tn, "_e", "_em", "_emi") then return "emissive" end
    -- (MRO / MRAO = metal, roughness, occlusion: the same channel order as MRA)
    for token, kind in pairs({ orm = "orm", arm = "orm", aorm = "orm", mra = "mra", mro = "mra", mrao = "mra", rma = "rma", rmo = "rma", rmao = "rma" }) do
        if pn == token or Ends(tn, "_" .. token) or Ends(pn, "_" .. token) then return kind end
    end
    if has("roughness") or Ends(tn, "_r", "_rough") then return "rough" end
    if has("metallic", "metalness") or Ends(tn, "_metal") then return "metal" end
    if has("ambientocclusion", "occlusion") or Ends(tn, "_ao") then return "ao" end
    if comp ~= 1 and srgb ~= false and (has("basecolor", "base color", "base_color", "albedo", "diffuse", "colour", "color")
        or Ends(tn, "_d", "_bc", "_co", "_c", "_col", "_diff", "_albedo", "_basecolor", "_diffuse") or pn == "bc" or pn == "co" or pn == "d") then return "colour" end
    if comp == 2 or comp == 3 or srgb == false then return "mask" end      -- linear / masks / greyscale: a packed mask we can't read
    if srgb == true and (comp == 0 or comp == 11 or comp == nil) then return "colour?" end
    return "unknown"
end
-- the texture slots of a material instance and its parent instances: { {param, kind, texName}, ... } (nil if unreadable)
local slotCache = {}
local function MaterialSlots(mi)
    local key = ObjectPath(mi)
    if slotCache[key] ~= nil then return slotCache[key] or nil end
    local out, seen, cur, depth, readable = {}, {}, mi, 0, false
    while cur and Valid(cur) and depth < 6 do
        local arr = nil
        pcall(function() arr = cur.TextureParameterValues end)
        if arr == nil then break end
        readable = true
        pcall(function() arr:ForEach(function(_, e)
            local v = e:get()
            local name = nil
            pcall(function() name = v.ParameterInfo.Name:ToString() end)
            if not name then pcall(function() name = v.ParameterName:ToString() end) end
            local tex = nil; pcall(function() tex = v.ParameterValue end)
            if name and name ~= "None" and not seen[name] and Valid(tex) then
                seen[name] = true
                local tn = "?"; pcall(function() tn = tex:GetFName():ToString() end)
                table.insert(out, { param = name, kind = TexKind(name, tex), tex = tn })
            end
        end) end)
        local par = nil; pcall(function() par = cur.Parent end)
        cur, depth = par, depth + 1
    end
    slotCache[key] = (readable and #out > 0) and out or false
    return slotCache[key] or nil
end
local MASK_FILL = { orm = "orm", mra = "mra", rma = "rma", rough = "rough", metal = "metal", ao = "ao", emissive = "black" }
-- ---- a report of what each imported model part got, for working out problems (in the bridge folder)
local reportLines = 0
local function Report(line)
    if not Dir then return end
    local path = Dir .. "materials_report.txt"
    if reportLines == 0 then local f = io.open(path, "w"); if f then f:write("UEPRO imported-model materials report\n"); f:close() end end
    if reportLines > 4000 then return end
    reportLines = reportLines + 1
    local f = io.open(path, "a"); if f then f:write(line, "\n"); f:close() end
end
E.Report = Report
-- what a borrowed material really does: its blend mode and two-sidedness (the instance's overrides first, then its base material)
local function MaterialLookOf(mi)
    local blend, two, domain = 0, false, 0
    local okB, baseMat = pcall(function() return mi:GetBaseMaterial() end)
    if okB and Valid(baseMat) then
        pcall(function() domain = tonumber(baseMat.MaterialDomain) or 0 end)
        pcall(function() blend = tonumber(baseMat.BlendMode) or 0 end)
        pcall(function() two = baseMat.TwoSided == true end)
    end
    pcall(function()
        local o = mi.BasePropertyOverrides
        if o.bOverride_BlendMode then blend = tonumber(o.BlendMode) or blend end
        if o.bOverride_TwoSided then two = o.TwoSided == true end
    end)
    return blend, two, domain
end
-- how well a borrowed material's blend mode stands in for the one wanted
local function BlendFit(want, have)
    if want == have then return 40 end
    local t = {
        [0] = { [1] = -25, [2] = -80, [3] = -80, [4] = -80 },        -- opaque wanted
        [1] = { [0] = -40, [2] = -15, [3] = -60, [4] = -60 },        -- cut-out wanted: opaque ignores the holes
        [2] = { [0] = -60, [1] = -10, [3] = -20, [4] = -25 },        -- see-through wanted: a cut-out is the next best
        [3] = { [0] = -80, [1] = -50, [2] = -15, [4] = -40 },        -- glow (additive)
        [4] = { [0] = -80, [1] = -50, [2] = -15, [3] = -30 } }       -- darken (modulate)
    return (t[want] or t[0])[have] or -80
end
-- picks a game material to carry an imported model's texture, matching the original's blend mode and two-sidedness;
-- returns material, colour parameter, its blend mode, whether it's two-sided
E.GLOW_SLOTS = { "Emissive", "EmissiveMap", "EmissiveTexture", "Emissive Map", "Emissive Texture", "EmissiveColorMap", "Glow", "GlowMap", "SI" }
local function TexturedMaterial(comp, want, wantTwo, wantGlow)
    want = want or 0
    local key = want .. (wantTwo and "2" or "1") .. (wantGlow and "g" or "")
    if texPick[key] then local t = texPick[key]; return t[1], t[2], t[3], t[4], t[5] end
    if not micList then
        micList = {}
        pcall(function() for _, mi in ipairs(FindAllOf("MaterialInstanceConstant") or {}) do if #micList >= 2500 then break end; table.insert(micList, mi) end end)
    end
    local tested = 0
    local best, bestScore, bestParam, bestBlend, bestTwo, bestSlots = nil, -1e9, nil, 0, false, nil
    for _, mi in ipairs(micList) do
        if tested >= 200 then break end
        local okN, name = pcall(function() return mi:GetFName():ToString():lower() end)
        local bad = not okN
        if okN then
            for _, w in ipairs(BAD_NAMES) do
                -- glass / hair / eyes are just what a see-through or cut-out model needs
                local fine = want ~= 0 and (w == "glass" or w == "hair" or w == "eye" or w == "foliage" or w == "grass")
                if not fine and name:find(w, 1, true) then bad = true; break end
            end
        end
        if not bad then
            local blend, two, domain = MaterialLookOf(mi)
            local fit = BlendFit(want, blend)
            if domain == 0 and fit > -60 then
                tested = tested + 1
                local okM, mid = pcall(function() return comp:CreateDynamicMaterialInstance(0, mi, FName("UEPROProbe")) end)
                local slots = MaterialSlots(mi)
                if slots and okM and Valid(mid) then
                    -- the material's own slot list: pick its colour slot and count what else would need filling
                    local found, sure = nil, false
                    for _, sl in ipairs(slots) do if sl.kind == "colour" then found, sure = sl.param, true; break end end
                    if not found then for _, sl in ipairs(slots) do if sl.kind == "colour?" then found = sl.param; break end end end
                    if found then
                        local score = (sure and 100 or 88) + fit
                        local hasNormal, hasOpac = false, false
                        for _, sl in ipairs(slots) do
                            if sl.kind == "normal" then hasNormal = true
                            elseif sl.kind == "opacity" then hasOpac = true
                            elseif MASK_FILL[sl.kind] then score = score - 2
                            elseif sl.param ~= found and (sl.kind == "mask" or sl.kind == "unknown" or sl.kind == "colour?" or sl.kind == "colour") then score = score - 15 end
                        end
                        if hasNormal then score = score + 6 end
                        if wantGlow then for _, sl in ipairs(slots) do if sl.kind == "emissive" then score = score + 25; break end end end   -- the model's glow picture can go here
                        if want ~= 0 and hasOpac then score = score + 10 end
                        if wantTwo == two then score = score + 8 elseif two then score = score - 4 else score = score - 3 end
                        for _, w in ipairs({ "master", "base", "default", "simple", "generic", "basic", "prop", "standard" }) do if name:find(w, 1, true) then score = score + 6; break end end
                        for _, w in ipairs({ "door", "metal", "chrome", "steel", "car", "vehicle", "weapon", "gun", "screen", "test", "debug" }) do if name:find(w, 1, true) then score = score - 12; break end end
                        if score > bestScore then best, bestScore, bestParam, bestBlend, bestTwo, bestSlots = mi, score, found, blend, two, slots end
                    end
                elseif okM and Valid(mid) then
                    local found, rank = nil, 0
                    for r, pn in ipairs(TEX_PARAMS) do
                        local okT, tex = pcall(function() return mid:K2_GetTextureParameterValue(FName(pn)) end)
                        if okT and Valid(tex) then found, rank = pn, r; break end
                    end
                    if found then
                        local score = 100 - rank * 2 + fit
                        local function has(pn) local okT, tex = pcall(function() return mid:K2_GetTextureParameterValue(FName(pn)) end); return okT and Valid(tex) end
                        for _, on in ipairs(NORMAL_SLOTS) do if has(on) then score = score + 6; break end end        -- the model's own normal map can go here
                        for on in pairs(NEUTRAL_SLOTS) do if has(on) then score = score - 2 end end                    -- filled with neutral values
                        for _, on in ipairs(UNKNOWN_SLOTS) do if has(on) then score = score - 15 end end              -- can't be made neutral
                        if want ~= 0 then for _, on in ipairs(OPACITY_SLOTS) do if has(on) then score = score + 10; break end end end   -- the model's alpha can go here
                        if wantGlow then for _, on in ipairs(E.GLOW_SLOTS) do if has(on) then score = score + 25; break end end end     -- the model's glow picture can go here
                        if wantTwo == two then score = score + 8 elseif two then score = score - 4 else score = score - 3 end   -- one-sided: back faces get added
                        local okS, metal = pcall(function() return mid:K2_GetScalarParameterValue(FName("Metallic")) end)
                        if okS and tonumber(metal) and metal > 0.3 then score = score - 30 end
                        for _, w in ipairs({ "master", "base", "default", "simple", "generic", "basic", "prop", "standard" }) do if name:find(w, 1, true) then score = score + 6; break end end
                        for _, w in ipairs({ "door", "metal", "chrome", "steel", "car", "vehicle", "weapon", "gun", "screen" }) do if name:find(w, 1, true) then score = score - 12; break end end
                        if score > bestScore then best, bestScore, bestParam, bestBlend, bestTwo, bestSlots = mi, score, found, blend, two, nil end
                    end
                end
            end
        end
        if best and bestScore >= (wantGlow and 185 or 160) then break end            -- right blend mode, two-sidedness and slots: can't do better
    end
    texPick[key] = { best, bestParam, bestBlend, bestTwo, bestSlots }
    if best then
        local d = {}
        for _, sl in ipairs(bestSlots or {}) do table.insert(d, sl.param .. "=" .. sl.kind .. "(" .. sl.tex .. ")") end
        Report(string.format("stand-in for %s%s: %s  colour slot %s  slots: %s", BLEND_NAMES[want] or "?", wantTwo and " two-sided" or "", ObjectPath(best), tostring(bestParam),
            bestSlots and table.concat(d, ", ") or "(not readable: guessed by name)"))
    else Report("no stand-in material found for " .. (BLEND_NAMES[want] or "?") .. " models") end
    print(string.format("[LauncherBridge] [Editor] material for %s%s models: %s\n", BLEND_NAMES[want] or "?", wantTwo and " two-sided" or "",
        best and (ObjectPath(best) .. " (" .. tostring(bestParam) .. ", " .. tostring(BLEND_NAMES[bestBlend]) .. (bestTwo and ", two-sided" or "") .. ", score " .. bestScore .. ")") or "none found, using vertex colours"))
    return best, bestParam, bestBlend, bestTwo, bestSlots
end
-- take the borrowed material's own look out (metal, shine, tints, glow), so only the model's texture shows
local defaultNormal, defaultNormalTried = nil, false
-- what a material setting does, from its name: "tint" (multiplies the colour), "emissive" (glow), "rough", "metal",
-- "grime" (dirt / wet / snow layers), "" (something else). Used to clear a borrowed material's own look, and to carry
-- material-editor changes saved with a model over to another game's materials.
function E.ParamRole(kind, name)
    local n = tostring(name or ""):lower()
    local function has(...) for _, w in ipairs({ ... }) do if n:find(w, 1, true) then return true end end return false end
    if has("dirt", "wet", "snow", "dust", "grime", "blood", "moss", "rain", "frost", "mud", "puddle", "damage") then return "grime" end
    if has("emiss", "emission", "glow", "selfillum", "self illum") or n:match("^si[ _/]") or n == "si" then return "emissive" end
    if kind == "vec" then
        if has("subsurface", "sss", "fresnel", "rim", "fog", "sheen", "fuzz", "edge", "scatter", "spec", "outline", "shadow", "sky", "water", "light") then return "" end
        if has("tint", "multipl", " mul", "_mul", "color", "colour", "albedo", "basecolor", "diffuse") or n == "co mul" then return "tint" end
        return ""
    end
    if kind == "sca" then
        if has("rough") then return "rough" end
        if has("metal") then return "metal" end
        return ""
    end
    return ""
end
local function Neutralise(mid, slotsKnown)
    for n, v in pairs(NEUTRAL_SCALARS) do pcall(function() mid:SetScalarParameterValue(FName(n), v) end) end
    -- every other tint / glow / grime setting the borrowed material has (its own look shouldn't colour the model)
    if E.MatParams then
        local ok, P = pcall(E.MatParams, mid)
        if ok and P then
            for _, e in ipairs(P.vec) do
                local r = E.ParamRole("vec", e.name)
                if r == "tint" then pcall(function() mid:SetVectorParameterValue(FName(e.name), { R = 1, G = 1, B = 1, A = 1 }) end)
                elseif r == "emissive" or r == "grime" then pcall(function() mid:SetVectorParameterValue(FName(e.name), { R = 0, G = 0, B = 0, A = 1 }) end) end
            end
            for _, e in ipairs(P.sca) do
                local r, n = E.ParamRole("sca", e.name), e.name:lower()
                if (r == "emissive" and (n:find("intens") or n:find("strength") or n:find("amount") or n:find("power") or n:find("bright") or n:find("mul") or n == "emissive"))
                    or (r == "grime" and (n:find("amount") or n:find("intens") or n:find("strength") or n:find("opacity") or n:find("blend") or n:find("mul"))) then
                    pcall(function() mid:SetScalarParameterValue(FName(e.name), 0) end)
                end
            end
        end
    end
    for _, n in ipairs(NEUTRAL_COLOURS) do pcall(function() mid:SetVectorParameterValue(FName(n), { R = 1, G = 1, B = 1, A = 1 }) end) end
    for _, n in ipairs({ "EmissiveColor", "Emissive Color", "EmissiveColour" }) do pcall(function() mid:SetVectorParameterValue(FName(n), { R = 0, G = 0, B = 0, A = 1 }) end) end
    if not defaultNormalTried then
        defaultNormalTried = true
        local p = "/Engine/EngineMaterials/DefaultNormal.DefaultNormal"
        defaultNormal = StaticFindObject(p)
        if not Valid(defaultNormal) then pcall(function() LoadAsset(p) end); defaultNormal = StaticFindObject(p) end
        if not Valid(defaultNormal) then defaultNormal = nil end
    end
    if defaultNormal and not slotsKnown then
        for _, n in ipairs({ "Normal", "NormalMap", "Normal Map", "NormalTexture", "N", "NM" }) do pcall(function() mid:SetTextureParameterValue(FName(n), defaultNormal) end) end
    end
end
local texCache = {}
-- the picture file a texture made by LoadTextureFile came from (for "Export texture")
function E.TextureSource(texPath)
    for path, t in pairs(texCache) do
        if t and Valid(t) then local ok, p = pcall(ObjectPath, t); if ok and p == texPath then return path end end
    end
    return nil
end
local function LoadTextureFile(path)
    if texCache[path] ~= nil then return texCache[path] or nil end
    local tex = nil
    pcall(function()
        local krl = StaticFindObject("/Script/Engine.Default__KismetRenderingLibrary")
        tex = krl:ImportFileAsTexture2D(World(), path)
    end)
    texCache[path] = Valid(tex) and tex or false
    if not Valid(tex) then print("[LauncherBridge] [Editor] couldn't load texture " .. tostring(path) .. "\n") end
    return Valid(tex) and tex or nil
end
-- the real texture on a game material if we can, otherwise vertex colours
-- back faces for a two-sided original drawn with a one-sided material
local function DoubledPart(p)
    if p.doubled then return p.doubled end
    local d = { verts = {}, normals = {}, uvs = {}, colors = {}, lcolors = {}, tangents = {}, tris = {} }
    local n = #p.verts
    for _, k in ipairs({ "verts", "uvs", "colors", "lcolors", "normals", "tangents" }) do for i, v in ipairs(p[k]) do d[k][i] = v end end
    for i = 1, n do
        local nn = p.normals[i]
        d.verts[n + i] = p.verts[i]; d.uvs[n + i] = p.uvs[i]; d.colors[n + i] = p.colors[i]; d.lcolors[n + i] = p.lcolors[i]
        if nn then d.normals[n + i] = { X = -nn.X, Y = -nn.Y, Z = -nn.Z } end
        local t = p.tangents[i]
        if t then d.tangents[n + i] = { TangentX = { X = -t.TangentX.X, Y = -t.TangentX.Y, Z = -t.TangentX.Z }, bFlipTangentY = t.bFlipTangentY } end
    end
    for i, v in ipairs(p.tris) do d.tris[i] = v end
    local nt = #p.tris
    for i = 1, nt, 3 do d.tris[nt + i] = p.tris[i] + n; d.tris[nt + i + 1] = p.tris[i + 2] + n; d.tris[nt + i + 2] = p.tris[i + 1] + n end
    p.doubled = d
    return d
end
local function WhiteColours(g)
    if g.white then return g.white end
    local w = { lin = {}, byte = {} }
    for i = 1, #g.verts do w.lin[i] = { R = 1, G = 1, B = 1, A = 1 }; w.byte[i] = { R = 255, G = 255, B = 255, A = 255 } end
    g.white = w
    return w
end
function E.PartSection(comp, p, solid, doubled, white)
    local g = doubled and DoubledPart(p) or p
    local lcol, bcol = g.lcolors, g.colors
    if white then local w = WhiteColours(g); lcol, bcol = w.lin, w.byte end   -- the game's own material: plain vertex colours, as on the original mesh
    local tans = (#g.tangents == #g.verts) and g.tangents or {}
    -- colours go in as FLinearColor (plain floats, like the positions): CreateMeshSection_LinearColor takes
    -- (section, vertices, triangles, normals, UV0..UV3, colours, tangents, collision[, sRGB]); the byte-colour version is only the fallback
    local okB, e = pcall(function() comp:CreateMeshSection_LinearColor(0, g.verts, g.tris, g.normals, g.uvs, {}, {}, {}, lcol, tans, solid, false) end)
    if not okB then okB, e = pcall(function() comp:CreateMeshSection(0, g.verts, g.tris, g.normals, g.uvs, bcol, tans, solid) end) end
    if not okB then print("[LauncherBridge] model part " .. tostring(p.name) .. ": " .. tostring(e) .. "\n") end
    p.sectionDoubled, p.sectionWhite = doubled, white
    return okB
end
-- the model's own material, when this is the game it came from (or one that has the same material)
local origCache = {}
local function OriginalMaterial(path)
    if not path then return nil end
    if origCache[path] ~= nil then return origCache[path] or nil end
    local mat = StaticFindObject(path)
    if not Valid(mat) then pcall(function() LoadAsset(path) end); mat = StaticFindObject(path) end
    local ok = Valid(mat) and pcall(function() return mat:GetBaseMaterial() end)
    origCache[path] = ok and mat or false
    if ok then print("[LauncherBridge] [Editor] using the model's original material " .. path .. "\n") end
    return ok and mat or nil
end
E.OriginalMaterial = OriginalMaterial
-- the original material if the game has it; otherwise the original textures on a game material with the same blend mode; otherwise vertex colours
-- returns textured, doubled (back faces needed), original (the game's own material is on it)
-- the model's glow (self-illumination) on a stand-in material: em = { file, r, g, b (colour x strength), ar, ag, ab (the
-- picture's average colour) }. With a glow-picture slot the glow follows the picture; without one the whole surface
-- glows evenly, so it takes the picture's average colour and is kept softer.
function E.ApplyGlow(mid, em, picture)
    if not em then return "" end
    local r, g, b = em.r, em.g, em.b
    if not picture then r, g, b = r * (em.ar or 1) * 0.6, g * (em.ag or 1) * 0.6, b * (em.ab or 1) * 0.6 end
    -- (too bright is brought down as a whole, so the glow keeps its colour)
    local top, mx = picture and 60 or 10, math.max(r, g, b, 0)
    if mx > top then r, g, b = r * top / mx, g * top / mx, b * top / mx end
    r, g, b = math.max(0, r), math.max(0, g), math.max(0, b)
    local strength = math.max(r, g, b, 0.001)
    local vecs, scas = {}, {}
    local okP, P = pcall(E.MatParams, mid)
    if okP and P then
        for _, e in ipairs(P.vec) do if E.ParamRole("vec", e.name) == "emissive" then table.insert(vecs, e.name) end end
        for _, e in ipairs(P.sca) do
            local n = e.name:lower()
            if E.ParamRole("sca", e.name) == "emissive" and (n:find("intens") or n:find("strength") or n:find("amount") or n:find("power") or n:find("bright") or n:find("mul") or n == "emissive" or n == "si") then table.insert(scas, e.name) end
        end
    end
    local guessed = #vecs == 0 and #scas == 0
    if guessed then vecs = { "EmissiveColor", "Emissive Color", "EmissiveColour", "Emissive Tint", "EmissiveTint" }; scas = { "EmissiveIntensity", "Emissive Intensity", "EmissiveStrength", "Emissive Strength", "Emissive Power", "EmissivePower" } end
    local done = {}
    if #vecs > 0 and not guessed then
        -- a colour setting carries colour x strength; strength settings at 1
        for _, n in ipairs(vecs) do if pcall(function() mid:SetVectorParameterValue(FName(n), { R = r, G = g, B = b, A = 1 }) end) then table.insert(done, n) end end
        for _, n in ipairs(scas) do if pcall(function() mid:SetScalarParameterValue(FName(n), 1) end) then table.insert(done, n) end end
    elseif not guessed then
        -- only strength settings: the picture gives the colour
        for _, n in ipairs(scas) do if pcall(function() mid:SetScalarParameterValue(FName(n), strength) end) then table.insert(done, n) end end
    else
        local unit = { R = r / strength, G = g / strength, B = b / strength, A = 1 }
        for _, n in ipairs(vecs) do pcall(function() mid:SetVectorParameterValue(FName(n), unit) end) end
        for _, n in ipairs(scas) do pcall(function() mid:SetScalarParameterValue(FName(n), strength) end) end
    end
    return string.format("glow %.1f,%.1f,%.1f%s (%s)", r, g, b, picture and " x picture" or " even", guessed and "guessed names" or table.concat(done, ", "))
end
function E.ApplyModelMaterial(comp, m, p, vcMaterial)
    local orig = OriginalMaterial(p.mat and m.origs and m.origs[p.mat])
    if orig and pcall(function() comp:SetMaterial(0, orig) end) then
        Report(string.format("  %s part %s: its original material %s", m.name, tostring(p.name), ObjectPath(orig)))
        pcall(function() comp:SetVisibility(true, false) end)
        return true, false, true
    end
    local textured = false
    local em = p.mat and m.emis and m.emis[p.mat] or nil
    local png = p.mat and m.mats[p.mat] or (em and em.file) or nil
    local look = (p.mat and m.looks and m.looks[p.mat]) or { blend = 0, two = false, alpha = 1 }
    local doubled = false
    if png then
        local baseM, pname, haveBlend, haveTwo, slots = TexturedMaterial(comp, look.blend, look.two, em ~= nil)
        local tex = baseM and LoadTextureFile(png)
        if baseM and not tex then Report("  " .. m.name .. " part " .. tostring(p.name) .. ": couldn't load its picture " .. png) end
        if baseM and tex and slots then
            -- the material's real slots: each gets the matching picture from the original material
            local okM, mid = pcall(function() return comp:CreateDynamicMaterialInstance(0, baseM, FName("")) end)
            if okM and Valid(mid) then
                Neutralise(mid, true)
                local opng = m.masks and m.masks[p.mat]
                local npng = m.normals and m.normals[p.mat]
                local set = {}
                local function put(param, t, what) if t and pcall(function() mid:SetTextureParameterValue(FName(param), t) end) then table.insert(set, param .. "<-" .. what) end end
                local glowPic = false
                for _, sl in ipairs(slots) do
                    local k = sl.kind
                    if sl.param == pname then -- colour, last
                    elseif k == "emissive" and em and LoadTextureFile(em.file or png) then
                        put(sl.param, LoadTextureFile(em.file or png), "glow"); glowPic = true
                    elseif k == "normal" then
                        local nt = npng and LoadTextureFile(npng)
                        put(sl.param, nt or LoadTextureFile(m.dir .. "_neutral_normal.png"), nt and "normal map" or "flat normal")
                    elseif MASK_FILL[k] then
                        local fill = MASK_FILL[k]
                        local nt = (opng and MASK_KINDS[fill]) and LoadTextureFile((opng:gsub("%.png$", "")) .. MASK_KINDS[fill] .. ".png") or nil
                        put(sl.param, nt or LoadTextureFile(m.dir .. "_neutral_" .. fill .. ".png"), nt and ("original " .. fill) or ("plain " .. fill))
                    elseif k == "opacity" then
                        local at = look.alphaFile and LoadTextureFile(look.alphaFile) or LoadTextureFile(m.dir .. "_neutral_white.png")
                        put(sl.param, at, look.alphaFile and "alpha" or "solid")
                    elseif k == "colour" or k == "colour?" then
                        put(sl.param, tex, "colour")           -- a second colour layer (e.g. a blend layer): same picture
                    end
                end
                if haveBlend ~= 0 then
                    local op = look.blend == 0 and 1 or (look.alphaFile and 1 or look.alpha)
                    for _, n in ipairs({ "Opacity", "OpacityAmount", "Opacity Amount", "Alpha" }) do pcall(function() mid:SetScalarParameterValue(FName(n), op) end) end
                    for _, n in ipairs({ "OpacityMaskClipValue", "Opacity Mask Clip Value", "ClipValue", "AlphaClip", "Clip" }) do pcall(function() mid:SetScalarParameterValue(FName(n), look.clip or 0.333) end) end
                end
                put(pname, tex, "colour")
                if em then table.insert(set, E.ApplyGlow(mid, em, glowPic)) end
                Report(string.format("  %s part %s -> %s: %s", m.name, tostring(p.name), ObjectPath(baseM), table.concat(set, ", ")))
                textured = true
                doubled = look.two and not haveTwo
            end
        elseif baseM and tex then
            local okM, mid = pcall(function() return comp:CreateDynamicMaterialInstance(0, baseM, FName("")) end)
            if okM and Valid(mid) then
                Neutralise(mid)
                local opng = m.masks and m.masks[p.mat]
                for slot, kind in pairs(NEUTRAL_SLOTS) do
                    local nt = nil
                    if opng and MASK_KINDS[kind] then nt = LoadTextureFile((opng:gsub("%.png$", "")) .. MASK_KINDS[kind] .. ".png") end   -- the original's roughness / metal / AO
                    nt = nt or LoadTextureFile(m.dir .. "_neutral_" .. kind .. ".png")
                    if nt then pcall(function() mid:SetTextureParameterValue(FName(slot), nt) end) end
                end
                local npng = m.normals and m.normals[p.mat]
                local ntex = npng and LoadTextureFile(npng) or LoadTextureFile(m.dir .. "_neutral_normal.png")
                if ntex then for _, slot in ipairs(NORMAL_SLOTS) do pcall(function() mid:SetTextureParameterValue(FName(slot), ntex) end) end end
                -- see-through and cut-out: the original's alpha into the opacity slots; opaque: fully solid
                if haveBlend ~= 0 then
                    local atex = look.alphaFile and LoadTextureFile(look.alphaFile)
                    if not atex and look.blend == 0 then atex = LoadTextureFile(m.dir .. "_neutral_white.png") end
                    local fed = false
                    if atex then for _, slot in ipairs(OPACITY_SLOTS) do if pcall(function() mid:SetTextureParameterValue(FName(slot), atex) end) then fed = true end end end
                    local op = look.blend == 0 and 1 or (look.alphaFile and 1 or look.alpha)
                    for _, n in ipairs({ "Opacity", "OpacityAmount", "Opacity Amount", "Alpha" }) do pcall(function() mid:SetScalarParameterValue(FName(n), op) end) end
                    for _, n in ipairs({ "OpacityMaskClipValue", "Opacity Mask Clip Value", "ClipValue", "AlphaClip", "Clip" }) do pcall(function() mid:SetScalarParameterValue(FName(n), look.clip or 0.333) end) end
                end
                pcall(function() mid:SetTextureParameterValue(FName(pname), tex) end)
                if em then
                    local gt, glowPic = LoadTextureFile(em.file or png), false
                    if gt then for _, slot in ipairs(E.GLOW_SLOTS) do
                        local okT, cur = pcall(function() return mid:K2_GetTextureParameterValue(FName(slot)) end)
                        if okT and Valid(cur) and pcall(function() mid:SetTextureParameterValue(FName(slot), gt) end) then glowPic = true end
                    end end
                    Report("  " .. m.name .. " part " .. tostring(p.name) .. ": " .. E.ApplyGlow(mid, em, glowPic))
                end
                textured = true
                doubled = look.two and not haveTwo
            end
        end
    end
    if not textured and vcMaterial then pcall(function() comp:SetMaterial(0, vcMaterial) end) end
    -- a see-through layer with no picture of its own (an eye cover, a glass shell) would show as a solid grey skin: hide it
    local hide = not textured and look.blend >= 2
    pcall(function() comp:SetVisibility(not hide, false) end)
    return textured, doubled, false
end
-- the model's effects, when this game has them (an effect is part of the game it came from, so it only comes back there)
function E.SpawnModelFx(host, m)
    if not m.fx or #m.fx == 0 then return end
    local root = nil; pcall(function() root = host:K2_GetRootComponent() end)
    if not Valid(root) then return end
    local nlib = StaticFindObject("/Script/Niagara.Default__NiagaraFunctionLibrary")
    local gs = StaticFindObject("/Script/Engine.Default__GameplayStatics")
    local placed, missing = 0, 0
    local lights = 0
    for _, fx in ipairs(m.fx) do
      if fx.kind == "light" then
        -- kind|r,g,b|intensity|attenuation|inner|outer|width|height|units|radius
        local f = {}; for x in (fx.path .. "|"):gmatch("([^|]*)|") do table.insert(f, x) end
        local kind = f[1]
        local cls = StaticFindObject(kind == "spot" and "/Script/Engine.SpotLightComponent" or kind == "rect" and "/Script/Engine.RectLightComponent" or "/Script/Engine.PointLightComponent")
        if Valid(cls) then
            local ident = { Rotation = { X = 0, Y = 0, Z = 0, W = 1 }, Translation = { X = 0, Y = 0, Z = 0 }, Scale3D = { X = 1, Y = 1, Z = 1 } }
            local ok, c = pcall(function() return host:AddComponentByClass(cls, false, ident, false) end)
            if ok and Valid(c) then
                local r, g, b = (f[2] or ""):match("(%d+),(%d+),(%d+)")
                pcall(function() c:SetMobility(2) end)
                pcall(function() c:K2_SetRelativeLocationAndRotation(fx.loc, fx.rot, false, {}, true) end)
                pcall(function() c.IntensityUnits = tonumber(f[9]) or 0 end)
                pcall(function() c:SetLightColor({ R = (tonumber(r) or 255) / 255, G = (tonumber(g) or 255) / 255, B = (tonumber(b) or 255) / 255, A = 1 }, true) end)
                pcall(function() c:SetIntensity(tonumber(f[3]) or 5000) end)
                pcall(function() c:SetAttenuationRadius(tonumber(f[4]) or 1000) end)
                if kind == "spot" then
                    pcall(function() c:SetInnerConeAngle(tonumber(f[5]) or 0) end)
                    pcall(function() c:SetOuterConeAngle(tonumber(f[6]) or 44) end)
                elseif kind == "rect" then
                    pcall(function() c:SetSourceWidth(tonumber(f[7]) or 64) end)
                    pcall(function() c:SetSourceHeight(tonumber(f[8]) or 64) end)
                end
                if (tonumber(f[10]) or 0) > 0 then pcall(function() c:SetSourceRadius(tonumber(f[10])) end) end
                lights = lights + 1
            end
        end
      else
        local asset = StaticFindObject(fx.path)
        if not Valid(asset) then pcall(function() LoadAsset(fx.path) end); asset = StaticFindObject(fx.path) end
        local comp = nil
        if Valid(asset) then
            if fx.kind == "niagara" and Valid(nlib) then
                local ok, c = pcall(function() return nlib:SpawnSystemAttached(asset, root, FName("None"), fx.loc, fx.rot, 0, false, true, 0, true) end)
                if ok and Valid(c) then comp = c end
            elseif fx.kind == "cascade" and Valid(gs) then
                local ok, c = pcall(function() return gs:SpawnEmitterAttached(asset, root, FName("None"), fx.loc, fx.rot, fx.scale, 0, false, 0, true) end)
                if not (ok and Valid(c)) then ok, c = pcall(function() return gs:SpawnEmitterAttached(asset, root, FName("None"), fx.loc, fx.rot, 0, false) end) end
                if ok and Valid(c) then comp = c end
            end
        end
        if comp then
            pcall(function() comp:SetRelativeScale3D(fx.scale) end)
            placed = placed + 1
        else missing = missing + 1 end
      end
    end
    if Report then pcall(Report, string.format("  %s effects: %d placed, %d not in this game, %d lights", m.name, placed, missing, lights)) end
    if missing > 0 and placed == 0 then Msg(m.name .. ": its effects belong to the game it came from, so they only show there") end
end
-- In the game a model came from, its real pieces are still in the game's files: show those instead of the
-- rigid copy, so it bends properly (true skinning) and plays the game's own animations.
local function FindOrLoad(path)
    if not path or path == "" then return nil end
    local ok, o = pcall(function() return StaticFindObject(path) end)
    if ok and Valid(o) then return o end
    pcall(function() LoadAsset(path) end)
    ok, o = pcall(function() return StaticFindObject(path) end)
    if ok and Valid(o) then return o end
    return nil
end
local function PickAnim(m)
    for i, a in ipairs(m.anims) do if a.frames > 1 and tostring(a.name):lower():find("idle") then return i end end
    return #m.anims > 0 and 1 or nil
end
local function PlayReal(inst, i)
    local a = inst.m.anims[i]; if not a or not a.path then return false end
    local seq = FindOrLoad(a.path); if not seq then return false end
    local ok = pcall(function() inst.real:PlayAnimation(seq, true) end)
    if ok then inst.realAnim = a end
    return ok
end
local function BuildReal(m, file, loc, rot)
    if not m.real or #m.real == 0 or m.realFailed then return nil end
    local hasSk = false
    for _, r in ipairs(m.real) do if r.kind == "skeletal" then hasSk = true end end
    if not hasSk then return nil end          -- still models keep the copy (it has your chosen materials)
    local function Why(t) print("[LauncherBridge] [Editor] " .. m.name .. ": using the copy (" .. t .. ")\n") end
    local assets = {}
    for i, r in ipairs(m.real) do
        local a = FindOrLoad(r.path)
        if not a then m.realFailed = true; Why("not in this game: " .. tostring(r.path)); return nil end   -- from another game
        local want = r.kind == "skeletal" and "SkeletalMesh" or "StaticMesh"
        local okC, cn = pcall(function() return a:GetClass():GetFName():ToString() end)
        local isA = okC and cn == want
        if not isA then
            local clsObj = StaticFindObject("/Script/Engine." .. want)
            local okA, r2 = pcall(function() return a:IsA(clsObj) end)
            isA = okA and r2 == true
        end
        if not isA then m.realFailed = true; Why(tostring(r.path) .. " is a " .. tostring(cn)); return nil end
        assets[i] = a
    end
    local host = SpawnOf("/Script/Engine.StaticMeshActor", loc, rot)
    if not host then Why("couldn't add an actor"); return nil end
    pcall(function() host.StaticMeshComponent:SetMobility(2) end)
    local skCls = StaticFindObject("/Script/Engine.SkeletalMeshComponent")
    local smCls = StaticFindObject("/Script/Engine.StaticMeshComponent")
    local ident = { Rotation = { X = 0, Y = 0, Z = 0, W = 1 }, Translation = { X = 0, Y = 0, Z = 0 }, Scale3D = { X = 1, Y = 1, Z = 1 } }
    local comps, leader = {}, nil
    for i, r in ipairs(m.real) do
        local ok, c = pcall(function() return host:AddComponentByClass(r.kind == "skeletal" and skCls or smCls, false, ident, false) end)
        if not (ok and Valid(c)) then pcall(function() host:K2_DestroyActor() end); m.realFailed = true; Why("couldn't add a component: " .. tostring(c)); return nil end
        pcall(function() c:SetMobility(2) end)
        if r.kind == "skeletal" then
            local set = pcall(function() c:SetSkeletalMeshAsset(assets[i]) end)
            if not set then pcall(function() c:SetSkeletalMesh(assets[i], true) end) end
            if leader then
                if not pcall(function() c:SetLeaderPoseComponent(leader, false, false) end) then pcall(function() c:SetMasterPoseComponent(leader, false) end) end
            else leader = c end
        else
            pcall(function() c:SetStaticMesh(assets[i]) end)
        end
        pcall(function() c:K2_SetRelativeLocationAndRotation(r.loc, r.rot, false, {}, true) end)
        pcall(function() c:SetRelativeScale3D(r.scale) end)
        pcall(function() c:SetCollisionEnabled(1) end)                                  -- you can click it
        pcall(function() c:SetCollisionProfileName(FName("BlockAll"), true) end)
        table.insert(comps, c)
    end
    local path = ObjectPath(host)
    local inst = { m = m, comps = comps, file = file, t = 0, last = -1, real = leader }
    Models.live[path] = inst
    table.insert(Models.order, path)
    local pick = PickAnim(m); if pick then PlayReal(inst, pick) end
    E.SpawnModelFx(host, m)
    return host
end
-- builds a model at loc/rot/scale; returns the actor (or nil, reason)
function E.BuildModel(file, loc, rot, scale)
    local cls = StaticFindObject("/Script/ProceduralMeshComponent.ProceduralMeshComponent")
    if not Valid(cls) then return nil, "This game doesn't include Unreal's procedural mesh feature, so imported models can't be built in it." end
    local m, err = ParseModel(file)
    if not m then return nil, err end
    do
        local okR, real = pcall(BuildReal, m, file, loc, rot)
        if not okR then print("[LauncherBridge] [Editor] real model failed: " .. tostring(real) .. "\n") end
        if okR and real then
            if scale then pcall(function() real:SetActorScale3D(scale) end) end
            return real
        end
    end
    local host = SpawnOf("/Script/Engine.StaticMeshActor", loc, rot)
    if not host then return nil, "The game wouldn't add the model." end
    pcall(function() host.StaticMeshComponent:SetMobility(2) end)
    local mat = VertexColourMaterial()
    local ident = { Rotation = { X = 0, Y = 0, Z = 0, W = 1 }, Translation = { X = 0, Y = 0, Z = 0 }, Scale3D = { X = 1, Y = 1, Z = 1 } }
    local comps = {}
    -- big still models collide with their light copy (a hidden section) instead of every one of their triangles,
    -- which made the game lag near them; big ones converted before that get one box instead
    m.useColl = m.coll and #m.coll.tris >= 3 and not m.hulls and #m.anims == 0
    m.bigBox = not m.useColl and not m.hulls and #m.anims == 0 and (m.tris or 0) > 150000
    for i, p in ipairs(m.parts) do
        local ok, comp = pcall(function() return host:AddComponentByClass(cls, false, ident, false) end)
        if not ok or not Valid(comp) then
            pcall(function() host:K2_DestroyActor() end)
            return nil, "The game wouldn't build the model (imported models need Unreal Engine 4.25 or newer)."
        end
        pcall(function() comp:SetMobility(2) end)
        if m.skinned then pcall(function() comp:SetBoundsScale(3) end) end   -- its shape changes while it animates: don't let it be culled by its standing pose
        -- collision: the model's own collision shapes if it has them (FBX UCX_ meshes, on the first part); otherwise
        -- every triangle (big models built in the background so the game doesn't stall); animated ones get one box
        -- instead (moving and re-bending pieces that each have collision is far too slow)
        pcall(function() comp.bUseAsyncCooking = true end)
        local solid = not m.hulls and #m.anims == 0 and not m.useColl and not m.bigBox
        if m.useColl and i == 1 then
            local okC = pcall(function()
                comp:CreateMeshSection_LinearColor(1, m.coll.verts, m.coll.tris, {}, {}, {}, {}, {}, {}, {}, true, false)
                comp:SetMeshSectionVisible(1, false)
            end)
            if okC then
                pcall(function() comp:SetCollisionEnabled(3) end)
                pcall(function() comp:SetCollisionProfileName(FName("BlockAll"), true) end)
            else m.useColl = false; solid = not m.bigBox end                   -- (couldn't: every triangle after all)
        end
        if m.hulls and i == 1 then
            local okH = pcall(function()
                comp.bUseComplexAsSimpleCollision = false
                comp:ClearCollisionConvexMeshes()
                for _, h in ipairs(m.hulls) do comp:AddCollisionConvexMesh(h) end
            end)
            if okH then
                pcall(function() comp:SetCollisionEnabled(3) end)
                pcall(function() comp:SetCollisionProfileName(FName("BlockAll"), true) end)
            else solid = #m.anims == 0 end                                          -- (couldn't: every triangle after all)
        end
        p.solid = solid
        E.PartSection(comp, p, solid, false)
        if solid then
            pcall(function() comp:SetCollisionEnabled(3) end)                         -- query + physics: you bump into it and can click it
            pcall(function() comp:SetCollisionProfileName(FName("BlockAll"), true) end)
        end
        local _, dbl, own = E.ApplyModelMaterial(comp, m, p, mat)
        if dbl or own then E.PartSection(comp, p, solid, dbl, own) end   -- back faces for a two-sided original on a one-sided material; white vertex colours under the original material
        comps[i] = comp
    end
    if (#m.anims > 0 or m.bigBox) and m.bmin and not m.hulls then   -- one box to bump into and click it by
        pcall(function()
            local bcls = StaticFindObject("/Script/Engine.BoxComponent")
            local box = host:AddComponentByClass(bcls, false, ident, false)
            local c = { X = (m.bmin[1] + m.bmax[1]) / 2, Y = (m.bmin[2] + m.bmax[2]) / 2, Z = (m.bmin[3] + m.bmax[3]) / 2 }
            box:SetBoxExtent({ X = (m.bmax[1] - m.bmin[1]) / 2, Y = (m.bmax[2] - m.bmin[2]) / 2, Z = (m.bmax[3] - m.bmin[3]) / 2 }, false)
            box:K2_SetRelativeLocationAndRotation(c, Rot(0, 0, 0), false, {}, true)
            box:SetCollisionEnabled(3)                                              -- you bump into it and can click it
            box:SetCollisionProfileName(FName("BlockAll"), true)
        end)
    end
    E.SpawnModelFx(host, m)
    if #m.anims == 0 then pcall(E.AddWalkable, host, m.bmin, m.bmax) end   -- (moving models aren't stood on)
    if scale then pcall(function() host:SetActorScale3D(scale) end) end
    local path = ObjectPath(host)
    local inst = { m = m, comps = comps, file = file, t = 0, last = -1 }
    Models.live[path] = inst
    table.insert(Models.order, path)
    -- models that came with animations start moving straight away (an idle one if there is one; loops)
    if #m.anims > 0 then
        local pick = 1
        for i, a in ipairs(m.anims) do if a.frames > 1 and tostring(a.name):lower():find("idle") then pick = i; break end end
        local a = m.anims[pick]
        if a and a.frames > 1 and (m.skinned or #a.k >= a.frames * #m.parts) then inst.anim, inst.clock = a, os.clock() end
    end
    -- material changes saved with the model (made in the material editor, in this game or another)
    if E.ApplyModelEdits then pcall(E.ApplyModelEdits, host, inst) end
    return host
end
-- ---- real skinning for animated characters in other games. Bending tens of thousands of vertices every frame is far
-- too slow from here, so the launcher's helper inside the game does it: we tell it (down a pipe, and wait for the
-- answer) which model, which animation, what time, and where each piece's vertex buffer is, and it writes the bent
-- skin straight into those buffers. Then the pieces are told to redraw.
local Sk = { cmd = nil, rep = nil, nextTry = 0, ids = {}, lay = nil, ms = 0, n = 0, flip = 0 }
function Sk.Drop()
    pcall(function() if Sk.cmd then Sk.cmd:close() end end); pcall(function() if Sk.rep then Sk.rep:close() end end)
    Sk.cmd, Sk.rep, Sk.ids = nil, nil, {}
end
function Sk.Open()
    if Sk.cmd then return true end
    local now = os.clock(); if now < Sk.nextTry then return false end
    Sk.nextTry = now + 2
    local c = io.open("\\\\.\\pipe\\UE4SSLauncherSkinCmd", "w"); if not c then return false end
    local r = io.open("\\\\.\\pipe\\UE4SSLauncherSkinRep", "r"); if not r then c:close(); return false end
    Sk.cmd, Sk.rep, Sk.ids = c, r, {}
    return true
end
function Sk.Call(line)
    if not Sk.Open() then return nil end
    local ok, res = pcall(function()
        if not Sk.cmd:write(line, "\n") then return nil end
        if not Sk.cmd:flush() then return nil end
        return Sk.rep:read("l")
    end)
    if not ok or not res then Sk.Drop(); return nil end
    return res
end
-- where the position / normal / tangent sit inside one vertex of a procedural mesh, in this game's engine
function Sk.Layout()
    if Sk.lay ~= nil then return Sk.lay or nil end
    Sk.lay = false
    local st = StaticFindObject("/Script/ProceduralMeshComponent.ProcMeshVertex")
    if not Valid(st) then return nil end
    local off = {}
    st:ForEachProperty(function(pr) off[pr:GetFName():ToString()] = pr:GetOffset_Internal() end)
    if not (off.Position and off.Normal and off.UV2 and off.UV3) then return nil end
    local dbl = (off.Normal - off.Position) >= 24
    Sk.lay = { stride = off.UV3 + (off.UV3 - off.UV2), p = off.Position, n = off.Normal, t = off.Tangent or -1, dbl = dbl and 1 or 0 }
    print(string.format("[LauncherBridge] [Editor] skin layout: vertex %d bytes, position %d, normal %d, tangent %d%s\n", Sk.lay.stride, Sk.lay.p, Sk.lay.n, Sk.lay.t, dbl and " (doubles)" or ""))
    return Sk.lay
end
-- poses one model: a = its animation (nil: its own pose). true when the skin was written.
function Sk.Pose(inst, a, t)
    local L = Sk.Layout(); if not L then return false end
    if not Sk.Open() then return false end
    local id = Sk.ids[inst.file]
    if id == nil then
        local r = Sk.Call("L\t" .. inst.file)
        id = r and tonumber(r:match("^ok (%d+)")) or false
        if Sk.cmd then Sk.ids[inst.file] = id end
        if not id then print("[LauncherBridge] [Editor] skin: couldn't load " .. tostring(inst.file) .. ": " .. tostring(r) .. "\n") end
    end
    if not id then return false end
    local ai = -1
    if a then for i, x in ipairs(inst.m.anims) do if x == a then ai = i - 1 end end end
    local parts = { string.format("S %d %d %.4f %d %d %d %d %d %d", id, ai, t or 0, L.stride, L.p, L.n, L.t, L.dbl, #inst.comps) }
    for i, comp in ipairs(inst.comps) do
        local buf = comp.ProcMeshSections[1].ProcVertexBuffer
        parts[#parts + 1] = string.format("%d %d", buf:GetArrayDataAddress(), buf:GetArrayNum())
    end
    local r = Sk.Call(table.concat(parts, " "))
    if r ~= "ok" then
        if r and not inst.skinWarned then inst.skinWarned = true; print("[LauncherBridge] [Editor] skin: " .. tostring(r) .. " for " .. tostring(inst.file) .. "\n") end
        return false
    end
    -- redraw from the changed buffers (any change to this value makes the engine rebuild what it draws)
    Sk.flip = 1 - Sk.flip
    inst.flip = 1 - (inst.flip or 0)
    for _, comp in ipairs(inst.comps) do comp:SetCustomDepthStencilValue(inst.flip) end
    return true
end
local function PartsHome(inst)
    if inst.m.skinned then inst.toRest = true; return end
    local hit = {}
    for _, comp in ipairs(inst.comps) do
        pcall(function() comp:K2_SetRelativeLocationAndRotation({ X = 0, Y = 0, Z = 0 }, Rot(0, 0, 0), false, hit, true) end)
    end
end
-- the instance to control: the selected one if it's this model, otherwise the newest of this model
local function ModelInstance(file)
    local sel = Models.live[selectedPath]
    if sel and (file == nil or sel.file == file) then return sel, selectedPath end
    for i = #Models.order, 1, -1 do
        local p = Models.order[i]; local inst = Models.live[p]
        if inst and (file == nil or inst.file == file) and Valid(inst.comps[1]) then return inst, p end
    end
end
handlers.ed_modelspawn = function(arg)
    local file, at = arg:match("^([^\t]+)\t?(.*)$")
    local m, err = ParseModel(file)
    if not m then Msg(err, true); return end
    local nx, ny = (at or ""):match("^([%-%d%.]+),([%-%d%.]+)$")
    local loc, onSurface, crot = AimPoint(tonumber(nx), tonumber(ny))
    if onSurface then loc.Z = loc.Z - m.minZ end      -- stand it on the surface
    local a, why = E.BuildModel(file, loc, Rot(0, (crot and crot.Yaw or 0) + 180, 0))
    if not a then Msg(why, true); return end
    local path = ObjectPath(a)
    if E.LayModelAdded then pcall(E.LayModelAdded, a, file) end
    if E.UndoAdded then pcall(E.UndoAdded, path) end
    table.insert(spawned, { path = path, name = m.name })
    E.Select(path, a)
    local inst = Models.live[path]
    local playing = inst and (inst.realAnim or inst.anim)
    Msg("Added " .. m.name .. (inst and inst.real and " (the game's own model)" or "") ..
        (#m.anims > 0 and string.format(" (playing %s; it has %d animation%s, pick another from Models)", playing and playing.name or "nothing", #m.anims, #m.anims == 1 and "" or "s") or ""))
end
-- the launcher changed which material a model's slots use: re-read it and repaint every copy in the level
handlers.ed_modelmats = function(file)
    Models.cache[file] = nil
    local m, err = ParseModel(file)
    if not m then Msg(err, true); return end
    local vc, n = VertexColourMaterial(), 0
    for _, inst in pairs(Models.live) do
        if inst.file == file and not inst.real then      -- (the game's own model keeps its own materials)
            inst.m.mats, inst.m.normals, inst.m.masks, inst.m.looks, inst.m.emis = m.mats, m.normals, m.masks, m.looks, m.emis
            for i, comp in ipairs(inst.comps) do
                local p = inst.m.parts[i]
                if Valid(comp) and p then
                    local _, dbl, own = E.ApplyModelMaterial(comp, inst.m, p, vc)
                    if (dbl or false) ~= (p.sectionDoubled or false) or (own or false) ~= (p.sectionWhite or false) then E.PartSection(comp, p, p.solid ~= false, dbl, own) end
                end
            end
            n = n + 1
        end
    end
    Msg(n > 0 and string.format("Updated the material on %d cop%s of %s", n, n == 1 and "y" or "ies", m.name) or ("Material saved for " .. m.name))
end
-- Diagnostic: how fast this game takes new vertex positions for a procedural mesh (for real skinning of imported
-- models). Writes the timings to meshbench.txt in the bridge folder.
handlers.ed_meshbench = function(arg)
    local n = math.max(100, math.min(100000, tonumber(arg) or 10000))
    local cls = StaticFindObject("/Script/ProceduralMeshComponent.ProceduralMeshComponent")
    local pc, pawn = PlayerPC()
    local l = { X = 0, Y = 0, Z = -100000 }
    local host = SpawnOf("/Script/Engine.StaticMeshActor", l, Rot(0, 0, 0))
    if not (host and Valid(cls)) then Msg("bench: couldn't start", true); return end
    local ident = { Rotation = { X = 0, Y = 0, Z = 0, W = 1 }, Translation = { X = 0, Y = 0, Z = 0 }, Scale3D = { X = 1, Y = 1, Z = 1 } }
    local comp = host:AddComponentByClass(cls, false, ident, false)
    local res = {}
    local function T(name, f) local t0 = os.clock(); local ok, e = pcall(f); table.insert(res, string.format("%s: %.1f ms%s", name, (os.clock() - t0) * 1000, ok and "" or (" ERROR " .. tostring(e)))) end
    local verts, tris = {}, {}
    T("lua table " .. n, function() for i = 1, n do verts[i] = { X = i % 100, Y = (i // 100) % 100, Z = i // 10000 } end end)
    for i = 0, n - 3, 3 do tris[#tris + 1] = i; tris[#tris + 1] = i + 1; tris[#tris + 1] = i + 2 end
    T("create", function() comp:CreateMeshSection_LinearColor(0, verts, tris, {}, {}, {}, {}, {}, {}, {}, false, false) end)
    for k = 1, 3 do
        local v2 = {}
        T("lua skin-ish math " .. n, function() for i = 1, n do local p = verts[i]; v2[i] = { X = p.X * 0.99 + 1, Y = p.Y * 1.01, Z = p.Z + k } end end)
        T("update " .. k, function() comp:UpdateMeshSection_LinearColor(0, v2, {}, {}, {}, {}, {}, {}, {}) end)
    end
    pcall(function() host:K2_DestroyActor() end)
    local dir = Freecam and Freecam.dir or ""
    local f = io.open(dir .. "meshbench.txt", "w"); if f then f:write(table.concat(res, "\n") .. "\n"); f:close() end
    Msg("bench: " .. table.concat(res, "; "))
end
-- Diagnostic: the game's own "rails" (Stray) near the player, written to railinfo.txt in the bridge folder.
handlers.ed_railinfo = function(arg)
    local out = {}
    local pc, pawn = PlayerPC()
    local pl = Valid(pawn) and pawn:K2_GetActorLocation() or { X = 0, Y = 0, Z = 0 }
    table.insert(out, string.format("player %.0f %.0f %.0f", pl.X, pl.Y, pl.Z))
    local radius = tonumber(arg) or 1500
    local n = 0
    for _, r in ipairs(FindAllOf("SplineRailComponent") or {}) do
        local ok, wl = pcall(function() return r:K2_GetComponentLocation() end)
        if ok and wl and math.abs(wl.X - pl.X) < radius and math.abs(wl.Y - pl.Y) < radius and math.abs(wl.Z - pl.Z) < radius and n < 25 then
            n = n + 1
            local owner = "?"; pcall(function() owner = r:GetOwner():GetFullName() end)
            local np = 0; pcall(function() np = r:GetNumberOfSplinePoints() end)
            local loop = "?"; pcall(function() loop = tostring(r:IsClosedLoop()) end)
            local w = "?"; pcall(function() w = tostring(r.Width) end)
            local rl, rr = "?", "?"; pcall(function() local v = r.RelativeLocation; rl = string.format("%.1f,%.1f,%.1f", v.X, v.Y, v.Z) end)
            pcall(function() local v = r.RelativeRotation; rr = string.format("%.1f,%.1f,%.1f", v.Pitch, v.Yaw, v.Roll) end)
            table.insert(out, string.format("rail %d owner=%s points=%d loop=%s width=%s rel=%s rot=%s world=%.0f,%.0f,%.0f", n, owner, np, loop, w, rl, rr, wl.X, wl.Y, wl.Z))
            -- every property of the component class itself (its own settings)
            pcall(function()
                local cls = r:GetClass()
                cls:ForEachProperty(function(prop)
                    local name = prop:GetFName():ToString()
                    local okv, v = pcall(function() return r[name] end)
                    local tv = okv and (type(v) == "number" or type(v) == "boolean" or type(v) == "string") and tostring(v) or (okv and type(v) or "?")
                    table.insert(out, "   prop " .. name .. " = " .. tv)
                end)
            end)
            for i = 0, math.min(np, 12) - 1 do
                local pt, wpt, up = nil, nil, nil
                pcall(function() pt = r:GetLocationAtSplinePoint(i, 0) end)
                pcall(function() wpt = r:GetLocationAtSplinePoint(i, 1) end)
                pcall(function() up = r:GetUpVectorAtSplinePoint(i, 1) end)
                table.insert(out, string.format("   pt %d local %s world %s up %s", i,
                    pt and string.format("%.1f,%.1f,%.1f", pt.X, pt.Y, pt.Z) or "?", wpt and string.format("%.0f,%.0f,%.0f", wpt.X, wpt.Y, wpt.Z) or "?",
                    up and string.format("%.2f,%.2f,%.2f", up.X, up.Y, up.Z) or "?"))
            end
            pcall(function()
                local o, e = {}, {}
                r:GetOwner():GetActorBounds(false, o, e, false)
                table.insert(out, string.format("   owner bounds centre %.0f,%.0f,%.0f half %.0f,%.0f,%.0f", o.X, o.Y, o.Z, e.X, e.Y, e.Z))
            end)
        end
    end
    local dir = Freecam and Freecam.dir or ""
    local f = io.open(dir .. "railinfo.txt", "w"); if f then f:write(table.concat(out, "\n") .. "\n"); f:close() end
    Msg(string.format("rails near you: %d (railinfo.txt)", n))
end
-- arg: model file \t animation number (1-based) | "stop"
handlers.ed_modelanim = function(arg)
    local file, what = arg:match("^([^\t]*)\t(.*)$")
    local inst = ModelInstance(file ~= "" and file or nil)
    if not inst then Msg("Add the model to the level first (Spawn).", true); return end
    if inst.real then
        if what == "stop" then pcall(function() inst.real:Stop() end); inst.realAnim = nil; Msg("Stopped"); return end
        if PlayReal(inst, tonumber(what) or 0) then Msg("Playing " .. inst.realAnim.name) else Msg("That animation isn't in this game.", true) end
        return
    end
    if what == "stop" then inst.anim = nil; PartsHome(inst); Msg("Stopped"); return end
    local a = inst.m.anims[tonumber(what) or 0]
    if not a or a.frames < 1 then Msg("That model has no animation with that number.", true); return end
    inst.anim, inst.t, inst.last = a, 0, -1
    inst.clock = os.clock()
    Msg("Playing " .. a.name)
end
function E.ModelsBusy()
    for _, inst in pairs(Models.live) do if inst.anim or inst.toRest then return true end end
    return false
end
function E.ModelsFrame()
    local now = os.clock()
    local hit = {}
    for path, inst in pairs(Models.live) do
        local a = inst.anim
        if a or inst.toRest then
            if not Valid(inst.comps[1]) then Models.live[path] = nil
            elseif inst.m.skinned then
                if a then inst.t = inst.t + math.min(0.25, now - (inst.clock or now)); inst.clock = now end
                local t0 = os.clock()
                local ok, done = pcall(Sk.Pose, inst, a, inst.t)
                if not ok and not inst.skinWarned then inst.skinWarned = true; print("[LauncherBridge] [Editor] skin error: " .. tostring(done) .. "\n") end
                Sk.ms = Sk.ms + (os.clock() - t0) * 1000; Sk.n = Sk.n + 1
                if ok and done then inst.toRest = nil end
            elseif a then
                inst.t = inst.t + math.min(0.25, now - (inst.clock or now)); inst.clock = now
                local fr = math.floor(inst.t * a.fps) % a.frames
                if fr ~= inst.last then
                    inst.last = fr
                    local base = fr * #inst.m.parts
                    for i, comp in ipairs(inst.comps) do
                        local k = a.k[base + i]
                        if k then pcall(function() comp:K2_SetRelativeLocationAndRotation({ X = k[1], Y = k[2], Z = k[3] }, Rot(k[4], k[5], k[6]), false, hit, true) end) end
                    end
                end
            else inst.toRest = nil end
        end
    end
end

-- Copies the selected object's model next to it (same rotation and scale).
handlers.ed_dupe = function(arg)
    local src = FindActor(arg); if not src then Msg("That actor no longer exists.", true); return end
    local mesh = nil
    pcall(function()
        local cls = StaticFindObject("/Script/Engine.StaticMeshComponent")
        local c = src:GetComponentByClass(cls)
        if Valid(c) then mesh = c.StaticMesh end
    end)
    if not Valid(mesh) then Msg(Str(src:GetFName()) .. " has no model to copy.", true); return end
    local l, r, sc = src:K2_GetActorLocation(), src:K2_GetActorRotation(), src:GetActorScale3D()
    local off = 100
    pcall(function()
        local origin, extent = {}, {}
        src:GetActorBounds(false, origin, extent, false)
        if extent.Y and extent.Y > 0 and extent.Y < 1e5 then off = extent.Y * 2 + 20 end
    end)
    local a = SpawnMeshActor(mesh, { X = l.X, Y = l.Y + off, Z = l.Z }, Rot(r.Pitch, r.Yaw, r.Roll), { X = sc.X, Y = sc.Y, Z = sc.Z })
    if not a then Msg("The game wouldn't copy that model.", true); return end
    Added(a, mesh)
end


-- ---------------------------------------------------------------- snapping
-- Traces with the actor's own collision switched off, so it never hits itself.
local function TraceIgnoring(a, s, e)
    local okC, had = pcall(function() return a:GetActorEnableCollision() end)
    pcall(function() a:SetActorEnableCollision(false) end)
    local _, pawn = PlayerPC()
    local ctx = Valid(pawn) and pawn or ViewPC()
    local color = { R = 0, G = 0, B = 0, A = 0 }
    local hit, got = {}, false
    for _, channel in ipairs({ 0, 1 }) do
        hit = {}
        local ok, wasHit = pcall(function()
            return UEHelpers.GetKismetSystemLibrary():LineTraceSingle(ctx, s, e, channel, false, {}, 0, hit, true, color, color, 0.0)
        end)
        if ok and wasHit then got = true; break end
    end
    pcall(function() a:SetActorEnableCollision(not okC or had ~= false) end)
    if not got then return nil end
    local p = hit.ImpactPoint or hit.Location
    local n = hit.ImpactNormal or hit.Normal or { X = 0, Y = 0, Z = 1 }
    if not (p and p.X) then return nil end
    return { X = p.X, Y = p.Y, Z = p.Z }, { X = n.X or 0, Y = n.Y or 0, Z = n.Z or 1 }, HitActor(hit)
end
local function Bounds(a)
    local origin, extent = {}, {}
    local ok = pcall(function() a:GetActorBounds(false, origin, extent, false) end)
    if not ok then pcall(function() origin, extent = {}, {}; a:GetActorBounds(false, origin, extent) end) end
    if origin.X and extent.X and extent.X < 1e6 then return origin, extent end
    local l = a:K2_GetActorLocation()
    return { X = l.X, Y = l.Y, Z = l.Z }, { X = 0, Y = 0, Z = 0 }
end
-- Moves the actor so its bounding box rests on the surface at point p with normal n.
local function RestOn(a, p, n)
    MakeMovable(a)
    local o, e = Bounds(a)
    local l = a:K2_GetActorLocation()
    local nl = math.sqrt(n.X * n.X + n.Y * n.Y + n.Z * n.Z)
    if nl < 1e-3 then n, nl = { X = 0, Y = 0, Z = 1 }, 1 end
    n = { X = n.X / nl, Y = n.Y / nl, Z = n.Z / nl }
    -- how far the box reaches towards the surface (its "support" along the normal)
    local reach = math.abs(n.X) * e.X + math.abs(n.Y) * e.Y + math.abs(n.Z) * e.Z
    local c = { X = p.X + n.X * reach, Y = p.Y + n.Y * reach, Z = p.Z + n.Z * reach }
    local target = { X = l.X + (c.X - o.X), Y = l.Y + (c.Y - o.Y), Z = l.Z + (c.Z - o.Z) }
    local hit = {}
    a:K2_SetActorLocation(target, false, hit, true)
end
-- arg: path \t ground          -> drop straight down onto whatever is below
--      path \t at \t sx,sy     -> put it on the surface under that screen point (floor, table, wall...)
handlers.ed_snap = function(arg)
    local path, mode, at = arg:match("^([^\t]+)\t(%a+)\t?(.*)$")
    local a = path and FindActor(path); if not a then Msg("That actor no longer exists.", true); return end
    if mode == "ground" then
        local o, e = Bounds(a)
        local s = { X = o.X, Y = o.Y, Z = o.Z - e.Z + 1 }
        local p, n, other = TraceIgnoring(a, s, { X = o.X, Y = o.Y, Z = o.Z - e.Z - 200000 })
        if not p then Msg("There's nothing below " .. Str(a:GetFName()) .. " to snap to.", true); return end
        RestOn(a, p, { X = 0, Y = 0, Z = 1 })
        Msg("Snapped to the ground" .. (other and (" (" .. Str(other:GetFName()) .. ")") or ""))
    else
        local nx, ny = (at or ""):match("^([%-%d%.]+),([%-%d%.]+)$")
        nx, ny = tonumber(nx), tonumber(ny); if not nx then return end
        local vpc = ViewPC()
        local size = {}
        vpc:GetViewportSize(size, size)
        local w, h = tonumber(size.SizeX) or 0, tonumber(size.SizeY) or 0
        local loc, dir = {}, {}
        if w <= 0 or not vpc:DeprojectScreenPositionToWorld(nx * w, ny * h, loc, dir) or not loc.X then Msg("Couldn't work out where you clicked.", true); return end
        local s = { X = loc.X, Y = loc.Y, Z = loc.Z }
        local p, n, other = TraceIgnoring(a, s, Vadd(s, dir, 200000))
        if not p then Msg("Nothing there to put it on.", true); return end
        RestOn(a, p, n)
        Msg("Put " .. Str(a:GetFName()) .. " on " .. (other and Str(other:GetFName()) or "the surface"))
    end
end

handlers.ed_models = function(arg) E.ListModels(arg) end
handlers.ed_npcs = function(arg) E.ListNpcs(arg) end

-- ---------------------------------------------------------------- model pictures
-- A hidden "photo studio" high above the camera: one mesh actor + one scene-capture camera that only
-- sees that actor. Each request photographs one model and saves two PNGs (lit, and plain colour as a
-- fallback) for the launcher to show. Requests are "id\tpath" entries split by |; one model is done per tick.
local thumbQ, thumbPending, thumbDir = {}, false, nil
local studio = nil   -- { mesh = actor, cam = actor, rt = render target }

local function KRL()
    local k = nil
    pcall(function() k = StaticFindObject("/Script/Engine.Default__KismetRenderingLibrary") end)
    return k
end

local function MakeStudio()
    if studio and Valid(studio.mesh) and Valid(studio.cam) and Valid(studio.rt) then return studio end
    local world = World(); if not Valid(world) then return nil end
    local krl = KRL(); if not Valid(krl) then return nil end
    local far = { X = 0, Y = 0, Z = 0 }
    local rot = Rot(0, 0, 0)
    local function spawn(path) return SpawnOf(path, far, rot) end
    local m, c = spawn("/Script/Engine.StaticMeshActor"), spawn("/Script/Engine.SceneCapture2D")
    if not (m and c) then return nil end
    -- a key light and a fill light of our own, so every model is lit the same way wherever you are
    local lights = {}
    for i = 1, 2 do
        local l = spawn("/Script/Engine.PointLight")
        if l then
            pcall(function() l.PointLightComponent:SetMobility(2) end)
            pcall(function() l.PointLightComponent:SetCastShadows(false) end)
            table.insert(lights, l)
        end
    end
    pcall(function() m.StaticMeshComponent:SetMobility(2) end)
    pcall(function() m:SetActorEnableCollision(false) end)
    pcall(function() m:SetActorHiddenInGame(true) end)
    local rt = nil
    local black = { R = 0, G = 0, B = 0, A = 0 }
    local ok = pcall(function() rt = krl:CreateRenderTarget2D(world, 256, 256, 2, black, false) end)          -- 2 = RGBA8
    if not (ok and Valid(rt)) then pcall(function() rt = krl:CreateRenderTarget2D(world, 256, 256, 2, black, false, false) end) end
    if not Valid(rt) then return nil end
    local cc = c.CaptureComponent2D
    pcall(function() cc.bCaptureEveryFrame = false end)
    pcall(function() cc.bCaptureOnMovement = false end)
    pcall(function() cc.PrimitiveRenderMode = 2 end)    -- only render what's on the show-only list
    pcall(function() cc.FOVAngle = 30 end)
    cc.TextureTarget = rt
    pcall(function() cc:ClearShowOnlyComponents() end)
    pcall(function() cc:ShowOnlyActorComponents(m, false) end)
    pcall(function() cc:ShowOnlyActorComponents(m) end)
    studio = { mesh = m, cam = c, rt = rt, lights = lights }
    return studio
end

local function Photograph(id, path)
    local mesh = FindMesh(path)
    local function fail() WriteFile(thumbDir .. id .. ".fail", "x") end
    if not mesh then fail(); return end
    local st = MakeStudio(); if not st then fail(); return end
    local world, krl = World(), KRL()
    -- park the studio 600 m above the camera, where nothing else is around
    local cx, cy, cz = ViewLocation()
    local base = { X = cx, Y = cy, Z = cz + 60000 }
    local comp = st.mesh.StaticMeshComponent
    comp:SetStaticMesh(mesh)
    local hit = {}
    st.mesh:K2_SetActorLocationAndRotation(base, Rot(0, 0, 0), false, hit, true)
    -- frame the whole model from a three-quarter view
    local cxm, cym, czm, r = base.X, base.Y, base.Z, 100
    pcall(function()
        local b = mesh:GetBounds()
        if b and b.Origin and b.SphereRadius then
            cxm, cym, czm = base.X + b.Origin.X, base.Y + b.Origin.Y, base.Z + b.Origin.Z
            r = math.max(1, b.SphereRadius)
        end
    end)
    local dist = r / math.sin(math.rad(15)) * 1.02
    -- Unreal's own thumbnail angle (the content browser's default): a little above, from the front-right
    local yaw, pitch = math.rad(-157.5), math.rad(-11.25)
    local dx, dy, dz = math.cos(pitch) * math.cos(yaw), math.cos(pitch) * math.sin(yaw), math.sin(pitch)
    local camLoc = { X = cxm - dx * dist, Y = cym - dy * dist, Z = czm - dz * dist }
    st.cam:K2_SetActorLocationAndRotation(camLoc, Rot(math.deg(pitch), math.deg(yaw), 0), false, hit, true)
    -- key light above and to the side of the camera, softer fill from the other side (brightness grows with
    -- distance so small and huge models come out the same)
    local dm = dist / 100
    local spots = { { side = 0.7, up = 0.9, k = 1.0 }, { side = -0.9, up = 0.2, k = 0.35 } }
    for i, l in ipairs(st.lights or {}) do
        local sp = spots[i]
        local rx, ry = -dy, dx                                -- camera's right, flat
        local ll = { X = camLoc.X + (rx * sp.side) * dist, Y = camLoc.Y + (ry * sp.side) * dist, Z = camLoc.Z + sp.up * dist }
        pcall(function() l:K2_SetActorLocation(ll, false, hit, true) end)
        pcall(function()
            local lc = l.PointLightComponent
            lc:SetAttenuationRadius(dist * 4)
            lc:SetIntensity(3000 * dm * dm * sp.k)
        end)
    end
    local cc = st.cam.CaptureComponent2D
    st.mesh:SetActorHiddenInGame(false)
    local function shot(source, file)
        pcall(function() cc.CaptureSource = source end)
        cc:CaptureScene()
        krl:ExportRenderTarget(world, st.rt, thumbDir, file)
    end
    local ok = pcall(function()
        shot(2, id .. ".png")      -- lit, as it looks in the game
        shot(7, id .. "_b.png")    -- plain colour (used if the lit picture comes out dark)
    end)
    st.mesh:SetActorHiddenInGame(true)
    if not ok then fail() end
end

handlers.ed_thumbs = function(arg)
    for line in arg:gmatch("[^|]+") do        -- entries "id\tpath" separated by |
        local id, path = line:match("^(%w+)\t(.+)$")
        if id then table.insert(thumbQ, { id = id, path = path }) end
    end
end
handlers.ed_thumbdir = function(arg) thumbDir = arg end

-- one model picture (game thread)
function E.ThumbFrame()
    if #thumbQ == 0 or not thumbDir then return end
    local job = table.remove(thumbQ, 1)
    local ok, err = pcall(Photograph, job.id, job.path)
    if not ok then Log("thumbnail failed: " .. tostring(err)); pcall(function() WriteFile(thumbDir .. job.id .. ".fail", "x") end) end
end



-- ---------------------------------------------------------------- play as another character
-- "Play as" makes your player controller possess any character / NPC in the level. Most NPCs aren't set up
-- for player controls and have no camera, so (unless switched off) we give them a third-person follow camera
-- and drive them ourselves from WASD / mouse / controller. "Back to my character" undoes it all.
local poss = { active = false, orig = nil, npc = nil, cam = nil, pc = nil, thirdPerson = true, assist = true, last = nil, origTarget = nil }
local pkeys = {}
local function PK(name)
    if not pkeys[name] then pkeys[name] = { KeyName = UEHelpers.FindOrAddFName(name) } end
    return pkeys[name]
end

-- every pawn in the level except you, nearest first: n=path \t name \t class \t distance
function E.ListNpcs(filter)
    filter = (filter or ""):lower()
    local cx, cy, cz = ViewLocation()
    local _, me = PlayerPC()
    local rows = {}
    for _, a in ipairs(FindAllOf("Pawn") or {}) do
        if Valid(a) and a ~= me and a ~= poss.orig then
            local okN, name = pcall(function() return a:GetFName():ToString() end)
            local okC, cls = pcall(function() return a:GetClass():GetFName():ToString() end)
            name, cls = okN and name or "?", okC and cls or "?"
            local skip = cls:find("Spectator") or cls:find("DebugCamera") or cls == "DefaultPawn"
            if not skip and (filter == "" or name:lower():find(filter, 1, true) or cls:lower():find(filter, 1, true)) then
                local okL, l = pcall(function() return a:K2_GetActorLocation() end)
                if okL and l then
                    local dx, dy, dz = l.X - cx, l.Y - cy, l.Z - cz
                    table.insert(rows, { a = a, name = name, cls = cls, d = math.sqrt(dx * dx + dy * dy + dz * dz) })
                end
            end
        end
    end
    table.sort(rows, function(x, y) return x.d < y.d end)
    local lines = { "filter=" .. Clean(filter), "total=" .. #rows }
    for i = 1, math.min(#rows, 300) do
        local r = rows[i]
        local path = ObjectPath(r.a)
        actorCache[path] = r.a
        table.insert(lines, string.format("a=%s\t%s\t%s\t%.0f", path, Clean(r.name), Clean(r.cls), r.d))
    end
    WriteFile(Dir .. "npcs.txt", table.concat(lines, "\n") .. "\n")
    Msg(string.format("Found %d characters", #rows))
end

local function PossCamOff()
    if Valid(poss.cam) then pcall(function() poss.cam:K2_DestroyActor() end) end
    poss.cam = nil
end
local function PossCamOn()
    if Valid(poss.cam) then return end
    local cam = SpawnOf("/Script/Engine.CameraActor", poss.npc:K2_GetActorLocation(), Rot(0, 0, 0))
    if not cam then return end
    poss.cam = cam
    pcall(function() poss.pc:SetViewTargetWithBlend(cam, 0.0, 0, 0.0, false) end)
end

function E.PossessActive() return poss.active or (E.SwapActive and E.SwapActive()) end

handlers.ed_possess = function(path)
    local npc = FindActor(path)
    if not npc then Msg("That character no longer exists.", true); return end
    local pc, me = PlayerPC()
    if not Valid(pc) then Msg("No player controller right now.", true); return end
    if npc == me then Msg("You're already playing as that.", true); return end
    if not poss.active then poss.orig = me end
    PossCamOff()
    local ok, err = pcall(function() pc:Possess(npc) end)
    if not ok then Msg("The game wouldn't let you control " .. Str(npc:GetFName()) .. ": " .. tostring(err), true); return end
    local okP, now = pcall(function() return pc.Pawn end)
    if not (okP and now == npc) then Msg("The game wouldn't let you control " .. Str(npc:GetFName()) .. ".", true); return end
    poss.active, poss.npc, poss.pc, poss.last = true, npc, pc, nil
    -- start the camera looking the way the character faces
    pcall(function() local r = npc:K2_GetActorRotation(); pc:SetControlRotation(Rot(-10, r.Yaw, 0)) end)
    if poss.thirdPerson then PossCamOn() end
    Msg("You're now playing as " .. Str(npc:GetFName()))
end

handlers.ed_unpossess = function()
    if not poss.active then return end
    local pc, npc, orig = poss.pc, poss.npc, poss.orig
    PossCamOff()
    poss.active, poss.npc = false, nil
    if Valid(pc) and Valid(orig) then
        pcall(function() pc:Possess(orig) end)
        pcall(function() pc:SetViewTargetWithBlend(orig, 0.0, 0, 0.0, false) end)
    end
    -- give the NPC its AI back so it carries on as before
    if Valid(npc) then pcall(function() npc:SpawnDefaultController() end) end
    poss.orig = nil
    Msg("Back to your own character")
end

-- options: "tp=1 assist=1"
handlers.ed_possopts = function(arg)
    poss.thirdPerson = not (arg or ""):find("tp=0", 1, true)
    poss.assist = not (arg or ""):find("assist=0", 1, true)
    if E.SwapOpts then E.SwapOpts() end
    if poss.active then
        if poss.thirdPerson then PossCamOn()
        else PossCamOff(); pcall(function() poss.pc:SetViewTargetWithBlend(poss.npc, 0.0, 0, 0.0, false) end) end
    end
end

-- Every frame while playing as someone else (game thread).
function E.PossessFrame()
    if E.SwapActive and E.SwapActive() then E.SwapFrame(); return end
    if not poss.active then return end
    local pc, npc = poss.pc, poss.npc
    if not (Valid(pc) and Valid(npc)) then
        PossCamOff(); poss.active = false
        if E.SwapLost then E.SwapLost() end
        if Valid(pc) and Valid(poss.orig) then pcall(function() pc:Possess(poss.orig) end) end
        Msg("The character you were playing as is gone - back to your own.", true)
        return
    end
    local now = os.clock()
    local dt = poss.last and math.min(0.1, now - poss.last) or 0.016
    if poss.last and dt < 0.0005 then return end        -- already done this frame
    poss.last = now
    local rot = pc:GetControlRotation()
    local pitch, yaw = rot.Pitch, rot.Yaw
    if pitch > 180 then pitch = pitch - 360 end
    if poss.assist then
        local function down(k) local ok, v = pcall(function() return pc:IsInputKeyDown(PK(k)) end); return ok and v == true end
        local function axis(k) local ok, v = pcall(function() return pc:GetInputAnalogKeyState(PK(k)) end); return (ok and type(v) == "number") and v or 0 end
        -- look: mouse + right stick
        local t = {}
        if pcall(function() pc:GetInputMouseDelta(t, t) end) and type(t.DeltaX) == "number" then
            yaw = yaw + t.DeltaX * 2.5; pitch = pitch + t.DeltaY * 2.5
        end
        yaw = yaw + axis("Gamepad_RightX") * 150 * dt
        pitch = pitch + axis("Gamepad_RightY") * 110 * dt
        pitch = math.max(-80, math.min(80, pitch))
        pcall(function() pc:SetControlRotation(Rot(pitch, yaw, 0)) end)
        -- move: WASD + left stick, relative to where the camera looks
        local f = (down("W") and 1 or 0) - (down("S") and 1 or 0) + axis("Gamepad_LeftY")
        local r = (down("D") and 1 or 0) - (down("A") and 1 or 0) + axis("Gamepad_LeftX")
        local y = math.rad(yaw)
        if math.abs(f) > 0.05 or math.abs(r) > 0.05 then
            local mx, my = math.cos(y) * f - math.sin(y) * r, math.sin(y) * f + math.cos(y) * r
            local len = math.sqrt(mx * mx + my * my)
            if len > 1 then mx, my = mx / len, my / len end
            pcall(function() npc:AddMovementInput(Vec(mx, my, 0), 1.0, false) end)
        end
        if down("SpaceBar") or down("Gamepad_FaceButton_Bottom") then pcall(function() npc:Jump() end)
        else pcall(function() npc:StopJumping() end) end
    end
    -- third-person camera behind the character, pulled in if a wall is in the way
    if poss.thirdPerson and Valid(poss.cam) then
        local l = npc:K2_GetActorLocation()
        local h, dist = 60, 320
        pcall(function()
            local o, e = {}, {}
            npc:GetActorBounds(true, o, e, false)
            if e.Z and e.Z > 0 and e.Z < 5000 then h = e.Z * 0.75; dist = math.max(250, e.Z * 3.2) end
        end)
        local p, yy = math.rad(pitch), math.rad(yaw)
        local fx, fy, fz = math.cos(p) * math.cos(yy), math.cos(p) * math.sin(yy), math.sin(p)
        local head = { X = l.X, Y = l.Y, Z = l.Z + h }
        local want = { X = head.X - fx * dist, Y = head.Y - fy * dist, Z = head.Z - fz * dist }
        pcall(function()
            local hit = {}
            local color = { R = 0, G = 0, B = 0, A = 0 }
            if UEHelpers.GetKismetSystemLibrary():LineTraceSingle(npc, head, want, 1, false, {}, 0, hit, true, color, color, 0.0) then
                local hp = hit.Location or hit.ImpactPoint
                if hp and hp.X then
                    local k = 0.9
                    want = { X = head.X + (hp.X - head.X) * k, Y = head.Y + (hp.Y - head.Y) * k, Z = head.Z + (hp.Z - head.Z) * k }
                end
            end
        end)
        pcall(function() poss.cam:K2_TeleportTo(want, Rot(pitch, yaw, 0)) end)
    end
end


-- ---------------------------------------------------------------- model swap (like a GTA trainer's "change player model")
-- Pick any character type: a fresh one is spawned where you stand and you take control of it (with its own body,
-- animations and moves). Your real character is hidden and frozen in place until you swap back.
local swap = { spawned = nil, orig = nil, name = nil }
local function ClassChain(cls)
    local names, guard = {}, 0
    while Valid(cls) and guard < 40 do
        guard = guard + 1
        local ok, n = pcall(function() return cls:GetFName():ToString() end)
        if ok then names[n] = true end
        local okS, sup = pcall(function() return cls:GetSuperStruct() end)
        cls = okS and sup or nil
    end
    return names
end
local function NiceName(n) return ((n or ""):gsub("_C$", ""):gsub("^BP_", ""):gsub("_", " ")) end
local function SkipClass(n)
    return n:find("^SKEL_") or n:find("^REINST_") or n:find("^TRASHCLASS") or n:find("Spectator") or n:find("DebugCamera")
        or n == "DefaultPawn" or n == "Character" or n == "Pawn"
end
-- characters you can swap into: s=class path \t name \t where \t how many in the level
function E.ListSwap(filter)
    filter = (filter or ""):lower()
    local rows, byPath = {}, {}
    local _, me = PlayerPC()
    local function add(cls, where)
        local okN, n = pcall(function() return cls:GetFName():ToString() end)
        if not okN or SkipClass(n) then return end
        local path = FullName(cls):match("%s(%S+)$"); if not path then return end
        local r = byPath[path]
        if not r then
            if filter ~= "" and not n:lower():find(filter, 1, true) then return end
            r = { path = path, name = NiceName(n), where = where, count = 0 }
            byPath[path] = r; table.insert(rows, r)
        end
        if where == "level" then r.where = "level"; r.count = r.count + 1 end
    end
    -- what's walking around in the level now
    for _, a in ipairs(FindAllOf("Character") or {}) do
        if Valid(a) and a ~= me and a ~= swap.orig and a ~= swap.spawned then
            local okC, c = pcall(function() return a:GetClass() end)
            if okC and Valid(c) then add(c, "level") end
        end
    end
    -- character blueprints the game has loaded (but none in the level right now)
    pcall(function()
        for _, c in ipairs(FindAllOf("BlueprintGeneratedClass") or {}) do
            if Valid(c) then
                local chain = ClassChain(c)
                if chain["Character"] then add(c, "loaded") end
            end
        end
    end)
    table.sort(rows, function(a, b)
        if (a.where == "level") ~= (b.where == "level") then return a.where == "level" end
        return a.name:lower() < b.name:lower()
    end)
    local lines = { "filter=" .. Clean(filter), "total=" .. #rows }
    for i = 1, math.min(#rows, 1500) do
        local r = rows[i]
        table.insert(lines, string.format("s=%s\t%s\t%s\t%d", r.path, Clean(r.name), r.where, r.count))
    end
    WriteFile(Dir .. "swaplist.txt", table.concat(lines, "\n") .. "\n")
    Msg(string.format("Found %d character models", #rows))
end
handlers.ed_swaplist = function(arg) E.ListSwap(arg) end

local function FreezeOrig(a, frozen)
    if not Valid(a) then return end
    pcall(function() a:SetActorHiddenInGame(frozen) end)
    pcall(function() a:SetActorEnableCollision(not frozen) end)
    pcall(function() a.CharacterMovement:SetMovementMode(frozen and 0 or 1, 0) end)   -- None / Walking
    pcall(function() a:SetActorTickEnabled(not frozen) end)
end
local function DropSpawned()
    if Valid(swap.spawned) then pcall(function() swap.spawned:K2_DestroyActor() end) end
    swap.spawned = nil
end
function E.SwappedTo() return (swap.spawned and Valid(swap.spawned)) and swap.name or nil end
function E.SwapLost()                       -- the swapped character died / vanished: put you back where it was
    if not swap.orig then return end
    FreezeOrig(swap.orig, false)
    swap.orig, swap.spawned, swap.name = nil, nil, nil
end

-- The new character keeps its own (AI) controller, so its animation blueprint runs exactly as the game made it,
-- and you keep controlling your own character's controller - so nothing about your character changes and
-- mouse / stick look carry on working. We just steer the new body from your inputs and follow it with a camera.
local function Ctl(a)
    local c
    pcall(function() c = a.Controller end)
    if not Valid(c) then pcall(function() c = a:GetController() end) end
    return Valid(c) and c or nil
end
local function Puppet(npc)
    -- take its AI away completely, so it can't fight, chase or wander on its own: only you move it.
    -- (Its movement still runs without a controller, and its animations follow how it moves.)
    pcall(function() npc.AutoPossessAI = 0 end)                          -- Disabled: don't get a new AI later
    local c = Ctl(npc)
    if c then
        pcall(function() c.BrainComponent:StopLogic("Model swap") end)
        pcall(function() c:StopMovement() end)
        pcall(function() c:UnPossess() end)
        pcall(function() c:K2_DestroyActor() end)
    end
    local m; pcall(function() m = npc.CharacterMovement end)
    if Valid(m) then
        pcall(function() m.bRunPhysicsWithNoController = true end)
        pcall(function() m.bOrientRotationToMovement = true end)        -- turn to face where it walks
        pcall(function() m.bUseControllerDesiredRotation = false end)
        pcall(function() m.RotationRate = Rot(0, 600, 0) end)
    end
    pcall(function() npc.bUseControllerRotationYaw = false end)
    return true
end
local function SwapCamOff()
    if Valid(swap.cam) then pcall(function() swap.cam:K2_DestroyActor() end) end
    swap.cam = nil
end
local function SwapView()
    local pc = swap.pc; if not Valid(pc) then return end
    if poss.thirdPerson then
        if not Valid(swap.cam) then
            swap.cam = SpawnOf("/Script/Engine.CameraActor", swap.spawned:K2_GetActorLocation(), Rot(0, 0, 0))
        end
        if Valid(swap.cam) then pcall(function() pc:SetViewTargetWithBlend(swap.cam, 0.0, 0, 0.0, false) end) end
    else
        SwapCamOff()
        pcall(function() pc:SetViewTargetWithBlend(swap.spawned, 0.0, 0, 0.0, false) end)
    end
end
function E.SwapActive() return swap.spawned ~= nil end
function E.SwapOpts() if swap.spawned and Valid(swap.spawned) then SwapView() end end

handlers.ed_swap = function(path)
    local cls = ClassFor(path)
    if not cls then Msg("Couldn't load " .. path, true); return end
    if not ClassChain(cls)["Pawn"] then Msg((path:match("([^/%.]+)$") or path) .. " isn't a character.", true); return end
    if poss.active then handlers.ed_unpossess() end         -- (older "play as" left on)
    local pc, me = PlayerPC()
    if not Valid(pc) or not Valid(me) then Msg("No player character right now.", true); return end
    local orig = swap.orig or me
    local from = Valid(swap.spawned) and swap.spawned or orig
    local okL, l = pcall(function() return from:K2_GetActorLocation() end)
    local okR, r = pcall(function() return from:K2_GetActorRotation() end)
    if not okL then Msg("Couldn't find where you are.", true); return end
    local rot = Rot(0, okR and r.Yaw or 0, 0)
    local old = swap.spawned
    FreezeOrig(orig, true)
    if Valid(old) then pcall(function() old:SetActorEnableCollision(false) end); pcall(function() old:SetActorHiddenInGame(true) end) end
    local fullPath = FullName(cls):match("%s(%S+)$") or path
    local nw = SpawnOf(fullPath, { X = l.X, Y = l.Y, Z = l.Z + 10 }, rot) or SpawnOf(fullPath, { X = l.X, Y = l.Y, Z = l.Z + 120 }, rot)
    if not nw then
        if Valid(old) then pcall(function() old:SetActorEnableCollision(true) end); pcall(function() old:SetActorHiddenInGame(false) end)
        else FreezeOrig(orig, false) end
        Msg("The game wouldn't create " .. NiceName(cls:GetFName():ToString()) .. ".", true)
        return
    end
    Puppet(nw)
    if Valid(old) then pcall(function() old:K2_DestroyActor() end) end
    if not swap.orig then pcall(function() pc:SetIgnoreMoveInput(true) end) end   -- your keys move the new body, not your hidden one
    swap.orig, swap.spawned, swap.pc, swap.name = orig, nw, pc, NiceName(cls:GetFName():ToString())
    swap.last, swap.ownLook, swap.stuck = nil, false, 0
    SwapView()
    Msg("You're now " .. swap.name)
end

local function SwapEnd(l, r)
    local orig, pc = swap.orig, swap.pc
    SwapCamOff()
    FreezeOrig(orig, false)
    if l and Valid(orig) then pcall(function() orig:K2_TeleportTo(l, Rot(0, r and r.Yaw or 0, 0)) end) end
    if Valid(pc) then
        pcall(function() pc:SetIgnoreMoveInput(false) end)
        if Valid(orig) then pcall(function() pc:SetViewTargetWithBlend(orig, 0.0, 0, 0.0, false) end) end
    end
    swap.orig, swap.spawned, swap.name, swap.pc = nil, nil, nil, nil
end
function E.SwapLost()
    if not swap.orig then return end
    SwapEnd(nil, nil)
    Msg("The character you swapped to is gone - back to your own.", true)
end

handlers.ed_swapback = function()
    if not swap.orig then
        if poss.active then handlers.ed_unpossess() end
        return
    end
    local body = swap.spawned
    local l, r
    if Valid(body) then
        pcall(function() l = body:K2_GetActorLocation() end)
        pcall(function() r = body:K2_GetActorRotation() end)
        pcall(function() body:SetActorEnableCollision(false) end)
    end
    SwapEnd(l, r)
    if Valid(body) then pcall(function() body:K2_DestroyActor() end) end
    Msg("Back to your own character")
end

-- Every frame while swapped (game thread).
function E.SwapFrame()
    local npc, pc = swap.spawned, swap.pc
    if not (Valid(npc) and Valid(pc)) then E.SwapLost(); return end
    local now = os.clock()
    local dt = swap.last and math.min(0.1, now - swap.last) or 0.016
    if swap.last and dt < 0.0005 then return end
    swap.last = now
    local function down(k) local ok, v = pcall(function() return pc:IsInputKeyDown(PK(k)) end); return ok and v == true end
    local function axis(k) local ok, v = pcall(function() return pc:GetInputAnalogKeyState(PK(k)) end); return (ok and type(v) == "number") and v or 0 end
    local rot = pc:GetControlRotation()
    local pitch, yaw = rot.Pitch, rot.Yaw
    if pitch > 180 then pitch = pitch - 360 end
    -- look: normally your own character still turns the camera (mouse / right stick). If the game doesn't
    -- (some freeze look while the character can't move), we turn it ourselves.
    local t = {}
    local mdx, mdy = 0, 0
    if pcall(function() pc:GetInputMouseDelta(t, t) end) and type(t.DeltaX) == "number" then mdx, mdy = t.DeltaX, t.DeltaY end
    local rx, ry = axis("Gamepad_RightX"), axis("Gamepad_RightY")
    local trying = math.abs(mdx) + math.abs(mdy) > 0.3 or math.abs(rx) + math.abs(ry) > 0.3
    if swap.prevYaw and trying and not swap.ownLook then
        if math.abs(yaw - swap.prevYaw) < 0.001 and math.abs(pitch - swap.prevPitch) < 0.001 then swap.stuck = swap.stuck + 1 else swap.stuck = 0 end
        if swap.stuck > 12 then swap.ownLook = true end
    end
    if swap.ownLook then
        yaw = yaw + mdx * 2.5 + rx * 150 * dt
        pitch = math.max(-80, math.min(80, pitch + mdy * 2.5 + ry * 110 * dt))
        pcall(function() pc:SetControlRotation(Rot(pitch, yaw, 0)) end)
    end
    swap.prevYaw, swap.prevPitch = yaw, pitch
    -- move: WASD + left stick, relative to the camera
    local f = (down("W") and 1 or 0) - (down("S") and 1 or 0) + axis("Gamepad_LeftY")
    local r = (down("D") and 1 or 0) - (down("A") and 1 or 0) + axis("Gamepad_LeftX")
    local y = math.rad(yaw)
    if math.abs(f) > 0.1 or math.abs(r) > 0.1 then
        local mx, my = math.cos(y) * f - math.sin(y) * r, math.sin(y) * f + math.cos(y) * r
        local len = math.sqrt(mx * mx + my * my)
        local scale = math.min(1, len)
        if len > 0 then mx, my = mx / len, my / len end
        pcall(function() npc:AddMovementInput(Vec(mx, my, 0), scale, true) end)
    end
    if down("SpaceBar") or down("Gamepad_FaceButton_Bottom") then pcall(function() npc:Jump() end)
    else pcall(function() npc:StopJumping() end) end
    -- keep the camera: some games put their own camera back
    if poss.thirdPerson and Valid(swap.cam) then
        local okV, vt = pcall(function() return pc:GetViewTarget() end)
        if okV and vt ~= swap.cam then pcall(function() pc:SetViewTargetWithBlend(swap.cam, 0.0, 0, 0.0, false) end) end
        local l = npc:K2_GetActorLocation()
        local h, dist = 60, 320
        pcall(function()
            local o, e = {}, {}
            npc:GetActorBounds(true, o, e, false)
            if e.Z and e.Z > 0 and e.Z < 5000 then h = e.Z * 0.75; dist = math.max(250, e.Z * 3.2) end
        end)
        local p, yy = math.rad(pitch), math.rad(yaw)
        local fx, fy, fz = math.cos(p) * math.cos(yy), math.cos(p) * math.sin(yy), math.sin(p)
        local head = { X = l.X, Y = l.Y, Z = l.Z + h }
        local want = { X = head.X - fx * dist, Y = head.Y - fy * dist, Z = head.Z - fz * dist }
        pcall(function()
            local hit = {}
            local color = { R = 0, G = 0, B = 0, A = 0 }
            if UEHelpers.GetKismetSystemLibrary():LineTraceSingle(npc, head, want, 1, false, {}, 0, hit, true, color, color, 0.0) then
                local hp = hit.Location or hit.ImpactPoint
                if hp and hp.X then want = { X = head.X + (hp.X - head.X) * 0.9, Y = head.Y + (hp.Y - head.Y) * 0.9, Z = head.Z + (hp.Z - head.Z) * 0.9 } end
            end
        end)
        pcall(function() swap.cam:K2_TeleportTo(want, Rot(pitch, yaw, 0)) end)
    end
end

-- ---------------------------------------------------------------- camera view: first person / third person for any game
-- Our own camera follows your character every frame. First person sits at the eyes of your character's
-- real body (head hidden, so you see your body, arms and legs when you look down); third person is an
-- over-the-shoulder camera that keeps out of walls. "Game default" gives the game its own camera back.
local vm = { mode = "off", fov = 90, dist = 320, shoulder = 45, height = 10, bob = true, face = true,
             cam = nil, pawn = nil, saved = nil, headMesh = nil, headBone = nil, hiddenComps = {}, pos = nil, last = nil }
local HEADISH = { "head", "face", "hair", "hat", "helmet", "beard", "eyebrow", "eyelash", "lash", "teeth", "tongue", "eyes", "mask", "glasses", "cap", "hood" }
local function Lc(x) return (tostring(x or "")):lower() end
local function CompName(c) local ok, n = pcall(function() return c:GetFName():ToString() end); return ok and n or "" end
local function MainMesh(p)
    local m; pcall(function() m = p.Mesh end)
    if Valid(m) then return m end
    local best, bestN
    local ok, list = pcall(function() return p:K2_GetComponentsByClass(StaticFindObject("/Script/Engine.SkeletalMeshComponent")) end)
    if ok and list then
        for _, c in ipairs(list) do
            local okN, n = pcall(function() return c:GetNumBones() end)
            if okN and type(n) == "number" and (not bestN or n > bestN) then best, bestN = c, n end
        end
    end
    return best
end
local function FindHead(mesh)
    if not Valid(mesh) then return nil end
    for _, n in ipairs({ "head", "Head", "HEAD", "head_01", "Bip001-Head", "Bip01-Head", "Bip001 Head", "Bip01 Head", "J_Head", "j_head", "Head_M", "c_head" }) do
        local ok, i = pcall(function() return mesh:GetBoneIndex(FName(n)) end)
        if ok and type(i) == "number" and i >= 0 then return n end
    end
    local okN, n = pcall(function() return mesh:GetNumBones() end)
    if okN and type(n) == "number" then
        for i = 0, n - 1 do
            local okB, b = pcall(function() return mesh:GetBoneName(i):ToString() end)
            local l = okB and b:lower() or ""
            if l:find("head") and not l:find("end") and not l:find("top") and not l:find("nub") and not l:find("light") then return b end
        end
        for i = 0, n - 1 do
            local okB, b = pcall(function() return mesh:GetBoneName(i):ToString() end)
            if okB and b:lower():find("neck") then return b end
        end
    end
    return nil
end
local function HalfHeight(p) local ok, h = pcall(function() return p.CapsuleComponent:GetScaledCapsuleHalfHeight() end); return (ok and type(h) == "number" and h > 1) and h or 88 end

local function ViewRestorePawn()
    local p = vm.pawn
    if Valid(p) then
        if vm.saved then
            pcall(function() p.bUseControllerRotationYaw = vm.saved.useYaw end)
            pcall(function() p.CharacterMovement.bOrientRotationToMovement = vm.saved.orient end)
            pcall(function() p.CharacterMovement.bUseControllerDesiredRotation = vm.saved.desired end)
        end
        if Valid(vm.headMesh) and vm.headBone then pcall(function() vm.headMesh:UnHideBoneByName(FName(vm.headBone)) end) end
        for _, c in ipairs(vm.hiddenComps) do if Valid(c) then pcall(function() c:SetVisibility(true, false) end) end end
    end
    vm.saved, vm.headMesh, vm.headBone, vm.hiddenComps, vm.pawn, vm.pos = nil, nil, nil, {}, nil, nil
end
local function ViewApplyPawn(p)
    vm.pawn = p
    vm.saved = {}
    pcall(function() vm.saved.useYaw = p.bUseControllerRotationYaw end)
    pcall(function() vm.saved.orient = p.CharacterMovement.bOrientRotationToMovement end)
    pcall(function() vm.saved.desired = p.CharacterMovement.bUseControllerDesiredRotation end)
    if vm.mode == "fp" then
        -- your body turns with your view, like a real first-person game
        pcall(function() p.bUseControllerRotationYaw = true end)
        pcall(function() p.CharacterMovement.bOrientRotationToMovement = false end)
        pcall(function() p.CharacterMovement.bUseControllerDesiredRotation = false end)
        -- hide the head (and hair / hats / face parts) so the camera doesn't see the inside of it
        local mesh = MainMesh(p)
        local head = FindHead(mesh)
        if head then pcall(function() mesh:HideBoneByName(FName(head), 0) end); vm.headMesh, vm.headBone = mesh, head end
        local ok, list = pcall(function() return p:K2_GetComponentsByClass(StaticFindObject("/Script/Engine.PrimitiveComponent")) end)
        if ok and list then
            for _, c in ipairs(list) do
                if c ~= mesh then
                    local n = Lc(CompName(c))
                    local sock = ""; pcall(function() sock = Lc(c:GetAttachSocketName():ToString()) end)
                    local hit = false
                    for _, w in ipairs(HEADISH) do if n:find(w, 1, true) or sock:find(w, 1, true) then hit = true; break end end
                    if hit then
                        local vis = false; pcall(function() vis = c:IsVisible() end)
                        if vis then pcall(function() c:SetVisibility(false, false) end); table.insert(vm.hiddenComps, c) end
                    end
                end
            end
        end
    elseif vm.face then
        -- third person: turn to face where you walk
        pcall(function() p.bUseControllerRotationYaw = false end)
        pcall(function() p.CharacterMovement.bOrientRotationToMovement = true end)
        pcall(function() p.CharacterMovement.bUseControllerDesiredRotation = false end)
        pcall(function() p.CharacterMovement.RotationRate = Rot(0, 540, 0) end)
    end
end
local function ViewOff()
    ViewRestorePawn()
    local pc, me = PlayerPC()
    if Valid(vm.cam) then
        if Valid(pc) and Valid(me) then pcall(function() pc:SetViewTargetWithBlend(me, 0.0, 0, 0.0, false) end) end
        pcall(function() vm.cam:K2_DestroyActor() end)
    end
    vm.cam = nil
end
function E.ViewMode() return vm.mode end

handlers.ed_viewmode = function(arg)
    local old = vm.mode
    for k, v in (arg or ""):gmatch("(%w+)=([%w%.%-]+)") do
        if k == "mode" then vm.mode = v
        elseif k == "bob" then vm.bob = v == "1"
        elseif k == "face" then vm.face = v == "1"
        elseif tonumber(v) then vm[k] = tonumber(v) end
    end
    -- re-apply to the character with the new settings
    ViewRestorePawn()
    if vm.mode == "off" then
        ViewOff()
        if old ~= "off" then Msg("Camera: the game's own") end
    else
        if Valid(vm.cam) then pcall(function() vm.cam.CameraComponent.FieldOfView = vm.fov end) end
        if old ~= vm.mode then Msg(vm.mode == "fp" and "Camera: first person" or "Camera: third person") end
    end
end

-- Every frame (from the camera hook, or the timer job as a fallback).
function E.ViewFrame()
    if vm.mode == "off" then return end
    if (Freecam and Freecam.active) or (E.SwapActive and E.SwapActive()) or poss.active then
        if vm.pawn then ViewRestorePawn() end      -- something else owns the camera right now
        return
    end
    local pc, me = PlayerPC()
    if not (Valid(pc) and Valid(me)) then return end
    if vm.pawn ~= me then ViewRestorePawn(); ViewApplyPawn(me) end
    local now = os.clock()
    local dt = vm.last and math.min(0.1, math.max(0.0005, now - vm.last)) or 0.016
    vm.last = now
    if not Valid(vm.cam) then
        local okL, l = pcall(function() return me:K2_GetActorLocation() end)
        vm.cam = SpawnOf("/Script/Engine.CameraActor", okL and l or { X = 0, Y = 0, Z = 0 }, Rot(0, 0, 0))
        if not Valid(vm.cam) then return end
        pcall(function() vm.cam.CameraComponent.bConstrainAspectRatio = false end)   -- no black bars on wide screens
        pcall(function() vm.cam.CameraComponent.FieldOfView = vm.fov end)
        vm.pos = nil
    end
    local okV, vt = pcall(function() return pc:GetViewTarget() end)
    if okV and vt ~= vm.cam then pcall(function() pc:SetViewTargetWithBlend(vm.cam, 0.0, 0, 0.0, false) end) end
    local r = pc:GetControlRotation()
    local pitch, yaw = r.Pitch, r.Yaw
    if pitch > 180 then pitch = pitch - 360 end
    local cy, sy, cp, sp = math.cos(math.rad(yaw)), math.sin(math.rad(yaw)), math.cos(math.rad(pitch)), math.sin(math.rad(pitch))
    local l = me:K2_GetActorLocation()
    local want
    if vm.mode == "fp" then
        local half = HalfHeight(me)
        local eye = { X = l.X, Y = l.Y, Z = l.Z + half * 0.82 }
        if vm.headMesh and vm.headBone then
            pcall(function()
                local h = vm.headMesh:GetSocketLocation(FName(vm.headBone))
                if h and h.X then
                    if vm.bob then eye = { X = h.X, Y = h.Y, Z = h.Z }          -- follow the head as it moves (natural bob)
                    else eye.Z = h.Z end                                         -- steady: only the head's height
                end
            end)
        end
        -- in front of the face, a little further forward when looking down so you see your body, not your neck
        local fwd = 11 + math.max(0, -pitch) / 90 * 14
        want = { X = eye.X + cy * fwd, Y = eye.Y + sy * fwd, Z = eye.Z + 7 + vm.height * 0.2 }
        -- smooth the head's small jitters, keep big moves instant
        if vm.pos then
            local a = 1 - math.exp(-dt * (vm.bob and 22 or 35))
            local dx, dy, dz = want.X - vm.pos.X, want.Y - vm.pos.Y, want.Z - vm.pos.Z
            if dx * dx + dy * dy + dz * dz < 200 * 200 then
                want = { X = vm.pos.X + dx * a, Y = vm.pos.Y + dy * a, Z = vm.pos.Z + dz * a }
            end
        end
    else
        local half = HalfHeight(me)
        local pivot = { X = l.X, Y = l.Y, Z = l.Z + half * 0.65 + vm.height }
        -- shoulder offset to the right of the view
        local rx, ry = -sy, cy
        pivot = { X = pivot.X + rx * vm.shoulder, Y = pivot.Y + ry * vm.shoulder, Z = pivot.Z }
        local dist = vm.dist * math.max(0.6, half / 88)
        local target = { X = pivot.X - cp * cy * dist, Y = pivot.Y - cp * sy * dist, Z = pivot.Z - sp * dist }
        -- keep out of walls
        pcall(function()
            local hit, color = {}, { R = 0, G = 0, B = 0, A = 0 }
            if UEHelpers.GetKismetSystemLibrary():LineTraceSingle(me, pivot, target, 1, false, { me }, 0, hit, true, color, color, 0.0) then
                local hp = hit.Location or hit.ImpactPoint
                if hp and hp.X then target = { X = pivot.X + (hp.X - pivot.X) * 0.92, Y = pivot.Y + (hp.Y - pivot.Y) * 0.92, Z = pivot.Z + (hp.Z - pivot.Z) * 0.92 } end
            end
        end)
        want = target
        -- a little camera lag, like a spring arm
        if vm.pos then
            local a = 1 - math.exp(-dt * 14)
            local dx, dy, dz = want.X - vm.pos.X, want.Y - vm.pos.Y, want.Z - vm.pos.Z
            if dx * dx + dy * dy + dz * dz < 600 * 600 then
                want = { X = vm.pos.X + dx * a, Y = vm.pos.Y + dy * a, Z = vm.pos.Z + dz * a }
            end
        end
    end
    vm.pos = want
    pcall(function() vm.cam:K2_SetActorLocationAndRotation(want, Rot(pitch, yaw, 0), false, {}, true) end)
end

-- ---------------------------------------------------------------- turn into another character (appearance)
-- Your own character keeps its controls, camera and abilities but takes another character's look: body model,
-- materials, separate clothing / hair parts, attached items, size and animations. "Back to my look" undoes it.
local look = nil   -- what we changed, so it can be put back
local function Each(arr, fn)                            -- UE4SS TArray or plain Lua table
    if type(arr) == "table" then for _, v in ipairs(arr) do fn(v) end return end
    pcall(function() arr:ForEach(function(_, e) fn(e:get()) end) end)
end
local function ClassObj(path) local ok, c = pcall(function() return StaticFindObject(path) end); return ok and Valid(c) and c or nil end
local function Comps(a, path)
    local out, cls = {}, ClassObj(path)
    if not cls then return out end
    local ok, arr = pcall(function() return a:K2_GetComponentsByClass(cls) end)
    if ok and arr then Each(arr, function(c) if Valid(c) then table.insert(out, c) end end) end
    return out
end
local function MeshAsset(c)
    for _, k in ipairs({ "SkeletalMesh", "SkinnedAsset", "SkeletalMeshAsset" }) do
        local ok, v = pcall(function() return c[k] end)
        if ok and Valid(v) then return v end
    end
    local ok, v = pcall(function() return c:GetSkeletalMeshAsset() end)
    if ok and Valid(v) then return v end
    return nil
end
local function SetMeshAsset(c, m)
    if pcall(function() c:SetSkeletalMesh(m, true) end) then return true end
    if pcall(function() c:SetSkeletalMeshAsset(m) end) then return true end
    return pcall(function() c.SkeletalMesh = m end)
end
local function MainMesh(a)
    local ok, m = pcall(function() return a.Mesh end)              -- ACharacter's mesh
    if ok and Valid(m) and MeshAsset(m) then return m end
    local best, bestSize = nil, -1
    for _, c in ipairs(Comps(a, "/Script/Engine.SkeletalMeshComponent")) do
        if MeshAsset(c) then
            local okB, b = pcall(function() return c.Bounds end)
            local sz = (okB and b and b.SphereRadius) or 0
            if sz > bestSize then best, bestSize = c, sz end
        end
    end
    return best
end
local function Rel(c)
    local l, r, s = { X = 0, Y = 0, Z = 0 }, { Pitch = 0, Yaw = 0, Roll = 0 }, { X = 1, Y = 1, Z = 1 }
    pcall(function() local v = c.RelativeLocation; l = { X = v.X, Y = v.Y, Z = v.Z } end)
    pcall(function() local v = c.RelativeRotation; r = { Pitch = v.Pitch, Yaw = v.Yaw, Roll = v.Roll } end)
    pcall(function() local v = c.RelativeScale3D; s = { X = v.X, Y = v.Y, Z = v.Z } end)
    return l, r, s
end
local function SetRel(c, l, r, s)
    local hit = {}
    pcall(function() c:K2_SetRelativeLocationAndRotation(l, r, false, hit, true) end)
    pcall(function() c:SetRelativeScale3D(s) end)
end
local function HalfHeight(a)
    local ok, h = pcall(function() return a.CapsuleComponent:GetScaledCapsuleHalfHeight() end)
    return (ok and type(h) == "number") and h or nil
end
local function Mats(c)
    local out = {}
    local ok, n = pcall(function() return c:GetNumMaterials() end)
    for i = 0, ((ok and n) or 0) - 1 do local okM, m = pcall(function() return c:GetMaterial(i) end); out[i] = okM and m or nil end
    return out, (ok and n) or 0
end
local function SetMats(c, mats, n) for i = 0, n - 1 do if mats[i] then pcall(function() c:SetMaterial(i, mats[i]) end) end end end
local function AnimClass(c) local ok, v = pcall(function() return c.AnimClass end); return ok and Valid(v) and v or nil end
local function SetAnim(c, cls)
    if not cls then return end
    if not pcall(function() c:SetAnimInstanceClass(cls) end) then pcall(function() c:SetAnimClass(cls) end) end
end
local function Skeleton(m) local ok, v = pcall(function() return m.Skeleton end); return ok and v or nil end
-- share of their bones that your skeleton also has (by name): the pose is copied by bone name
local function SharedBones(theirs, mine)
    local okN, n = pcall(function() return theirs:GetNumBones() end)
    if not okN or type(n) ~= "number" or n <= 0 then return 0 end
    local hit = 0
    for i = 0, n - 1 do
        local okB, name = pcall(function() return theirs:GetBoneName(i) end)
        if okB and name then
            local okI, idx = pcall(function() return mine:GetBoneIndex(name) end)
            if okI and type(idx) == "number" and idx >= 0 then hit = hit + 1 end
        end
    end
    return hit / n
end
local function FollowPose(part, leader)
    if not pcall(function() part:SetLeaderPoseComponent(leader, false) end) then pcall(function() part:SetMasterPoseComponent(leader, false) end) end
end
local function AddPart(owner, classPath, attachTo, socket, l, r, s)
    local cls = ClassObj(classPath); if not cls then return nil end
    local tf = { Translation = l, Rotation = { X = 0, Y = 0, Z = 0, W = 1 }, Scale3D = s }
    local ok, c = pcall(function() return owner:AddComponentByClass(cls, true, tf, false) end)
    if not (ok and Valid(c)) then return nil end
    local rules = 1   -- KeepRelative
    pcall(function() c:K2_AttachToComponent(attachTo, socket, rules, rules, rules, true) end)
    SetRel(c, l, r, s)
    return c
end

local function LookAsActor(npc, displayName)
    local _, me = PlayerPC(); if not Valid(me) then Msg("No player character right now.", true); return end
    if look then handlers.ed_lookback("quiet") end
    local myMesh, theirMesh = MainMesh(me), MainMesh(npc)
    if not myMesh then Msg("Your character has no body model that can be changed.", true); return end
    if not theirMesh then Msg(Str(npc:GetFName()) .. " has no character model to copy.", true); return end
    local newAsset = MeshAsset(theirMesh)
    -- remember everything we're about to change
    local l, r, s = Rel(myMesh)
    local mats, n = Mats(myMesh)
    look = { pawn = me, mesh = myMesh, asset = MeshAsset(myMesh), mats = mats, nmats = n, anim = AnimClass(myMesh),
             l = l, r = r, s = s, hidden = {}, added = {}, name = displayName or Str(npc:GetFName()) }
    -- hide my other visible parts (clothes, hair, held items...)
    for _, path2 in ipairs({ "/Script/Engine.SkeletalMeshComponent", "/Script/Engine.StaticMeshComponent" }) do
        for _, c in ipairs(Comps(me, path2)) do
            if c ~= myMesh then
                local okV, vis = pcall(function() return c:IsVisible() end)
                if okV and vis then pcall(function() c:SetVisibility(false, false) end); table.insert(look.hidden, c) end
            end
        end
    end
    -- body model + materials. Your own animations always stay in charge.
    local sameSkeleton = Skeleton(newAsset) ~= nil and Skeleton(newAsset) == Skeleton(look.asset)
    local tm, tn = Mats(theirMesh)
    if sameSkeleton then
        -- same bones: just swap the model in place, your animation blueprint keeps driving it
        if not SetMeshAsset(myMesh, newAsset) then Msg("The game wouldn't change your character's model.", true); handlers.ed_lookback("quiet"); return end
        SetMats(myMesh, tm, tn)
    elseif SharedBones(theirMesh, myMesh) >= 0.5 then
        -- different skeleton but the bones share names: your (hidden) body keeps animating,
        -- and their model copies its pose bone-by-bone
        local body = AddPart(me, "/Script/Engine.SkeletalMeshComponent", myMesh, UEHelpers.FindOrAddFName("None"),
                             { X = 0, Y = 0, Z = 0 }, { Pitch = 0, Yaw = 0, Roll = 0 }, { X = 1, Y = 1, Z = 1 })
        if not body or not SetMeshAsset(body, newAsset) then
            if body then pcall(function() body:K2_DestroyComponent(body) end) end
            Msg("The game wouldn't add " .. look.name .. "'s model.", true); handlers.ed_lookback("quiet"); return
        end
        SetMats(body, tm, tn)
        FollowPose(body, myMesh)
        table.insert(look.added, body)
        -- hide your body but keep it animating (the new model needs its bones every frame)
        local okT, tick = pcall(function() return myMesh.VisibilityBasedAnimTickOption end)
        look.tick = okT and tick or nil
        pcall(function() myMesh.VisibilityBasedAnimTickOption = 0 end)   -- AlwaysTickPoseAndRefreshBones
        pcall(function() myMesh:SetVisibility(false, false) end)
        look.hidMain = true
    else
        -- a different kind of skeleton: their whole model with their own animation blueprint, standing where your
        -- body is (feet on the ground). It animates from how your character moves.
        local root; pcall(function() root = me:K2_GetRootComponent() end)
        local nl, nr, ns = Rel(theirMesh)
        local myHalf, theirHalf = 88, 88
        pcall(function() myHalf = me.CapsuleComponent:GetScaledCapsuleHalfHeight() end)
        pcall(function() theirHalf = npc.CapsuleComponent:GetScaledCapsuleHalfHeight() end)
        local pos = { X = nl.X, Y = nl.Y, Z = nl.Z + theirHalf - myHalf }
        local body = AddPart(me, "/Script/Engine.SkeletalMeshComponent", Valid(root) and root or myMesh, UEHelpers.FindOrAddFName("None"), pos, nr, ns)
        if not body or not SetMeshAsset(body, newAsset) then
            if body then pcall(function() body:K2_DestroyComponent(body) end) end
            Msg("The game wouldn't add " .. look.name .. "'s model.", true); handlers.ed_lookback("quiet"); return
        end
        SetMats(body, tm, tn)
        local ac = AnimClass(theirMesh)
        if ac then pcall(function() body:SetAnimInstanceClass(ac) end) end
        pcall(function() body.VisibilityBasedAnimTickOption = 0 end)
        table.insert(look.added, body)
        pcall(function() myMesh:SetVisibility(false, false) end)
        look.hidMain = true
        look.ownBody = body
    end
    -- their separate parts: clothing / hair pieces that follow the body, and items held on sockets
    for _, c in ipairs(Comps(npc, "/Script/Engine.SkeletalMeshComponent")) do
        if c ~= theirMesh and MeshAsset(c) then
            local okV, vis = pcall(function() return c:IsVisible() end)
            if okV and vis then
                local okS, sock = pcall(function() return c:GetAttachSocketName() end)
                local pl, pr, ps = Rel(c)
                local base = look.ownBody or myMesh
                local part = AddPart(me, "/Script/Engine.SkeletalMeshComponent", base, okS and sock or UEHelpers.FindOrAddFName("None"), pl, pr, ps)
                if part then
                    SetMeshAsset(part, MeshAsset(c))
                    local pm, pn = Mats(c); SetMats(part, pm, pn)
                    FollowPose(part, base)
                    table.insert(look.added, part)
                end
            end
        end
    end
    for _, c in ipairs(Comps(npc, "/Script/Engine.StaticMeshComponent")) do
        local okP, parent = pcall(function() return c:GetAttachParent() end)
        local okV, vis = pcall(function() return c:IsVisible() end)
        local okM, sm = pcall(function() return c.StaticMesh end)
        if okP and okV and vis and okM and Valid(sm) then
            local onBody = false
            pcall(function() onBody = parent == theirMesh or parent:IsA("/Script/Engine.SkeletalMeshComponent") end)
            if onBody then
                local okS, sock = pcall(function() return c:GetAttachSocketName() end)
                local pl, pr, ps = Rel(c)
                local part = AddPart(me, "/Script/Engine.StaticMeshComponent", look.ownBody or myMesh, okS and sock or UEHelpers.FindOrAddFName("None"), pl, pr, ps)
                if part then
                    pcall(function() part:SetStaticMesh(sm) end)
                    local pm, pn = Mats(c); SetMats(part, pm, pn)
                    pcall(function() part:SetCollisionEnabled(0) end)
                    table.insert(look.added, part)
                end
            end
        end
    end
    Msg("You're now " .. look.name .. (look.ownBody and "" or " (moving with your own animations)"))
    return true
end
handlers.ed_lookas = function(path)
    local npc = FindActor(path); if not npc then Msg("That character no longer exists.", true); return end
    LookAsActor(npc)
end

-- Model swap (like RDR2's Model Swap): you stay you - same controls, camera, health and story - with another
-- character's model. Uses one of that character in the level, or makes a temporary one out of sight to copy.
handlers.ed_modelswap = function(path)
    if E.SwapActive and E.SwapActive() then handlers.ed_swapback() end
    local cls = ClassFor(path)
    if not cls then Msg("Couldn't load " .. path, true); return end
    local full = FullName(cls):match("%s(%S+)$") or path
    local nice = NiceName(cls:GetFName():ToString())
    local _, me = PlayerPC()
    local src
    for _, a in ipairs(FindAllOf("Character") or {}) do
        if Valid(a) and a ~= me then
            local okC, c = pcall(function() return a:GetClass() end)
            if okC and c == cls then src = a; break end
        end
    end
    local temp = false
    if not src then
        -- none in the level: make one far below the map, copy it, and remove it straight away
        local l = { X = 0, Y = 0, Z = -50000 }
        pcall(function() local m = me:K2_GetActorLocation(); l = { X = m.X, Y = m.Y, Z = m.Z - 50000 } end)
        src = SpawnOf(full, l, Rot(0, 0, 0))
        if not src then Msg("The game wouldn't create " .. nice .. " to copy.", true); return end
        temp = true
        pcall(function() src:SetActorHiddenInGame(true) end)
        pcall(function() src:SetActorEnableCollision(false) end)
        pcall(function() src:SetActorTickEnabled(false) end)
        local c = Ctl(src); if c then pcall(function() c.BrainComponent:StopLogic("copy") end) end
    end
    if look then handlers.ed_lookback("quiet") end
    LookAsActor(src, nice)
    if temp then
        local c = Ctl(src)
        if c then pcall(function() c:UnPossess() end); pcall(function() c:K2_DestroyActor() end) end
        pcall(function() src:K2_DestroyActor() end)
    end
end

handlers.ed_lookback = function(arg)
    if not look then return end
    local L = look; look = nil
    for _, c in ipairs(L.added) do if Valid(c) then pcall(function() c:K2_DestroyComponent(c) end) end end
    if Valid(L.mesh) then
        if L.asset then SetMeshAsset(L.mesh, L.asset) end
        pcall(function() L.mesh:EmptyOverrideMaterials() end)   -- drop leftover slots from their model
        SetMats(L.mesh, L.mats, L.nmats)
        if L.hidMain then
            pcall(function() L.mesh:SetVisibility(true, false) end)
            if L.tick then pcall(function() L.mesh.VisibilityBasedAnimTickOption = L.tick end) end
        end
    end
    for _, c in ipairs(L.hidden) do if Valid(c) then pcall(function() c:SetVisibility(true, false) end) end end
    if arg ~= "quiet" then Msg("Back to your own look") end
end
function E.LookingAs() return look and look.name or nil end

function E.SpawnedLines()
    local out = {}
    for i = #spawned, 1, -1 do
        local s = spawned[i]
        local a = FindActor(s.path)
        if a and not E.softDeleted[s.path] then table.insert(out, "added=" .. s.path .. "\t" .. Clean(s.name)) else table.remove(spawned, i) end
    end
    return out
end

function E.Handle(verb, arg)
    local h = handlers[verb]
    if not h then return false end
    ExecuteInGameThread(function()
        local ok, err = pcall(h, arg or "")
        if not ok then Msg(verb .. " failed: " .. tostring(err), true) end
        if (E.open or E.watch) and verb ~= "ed_close" then pcall(E.WriteState) end
    end)
    return true
end

local tick = 0
-- Camera only, every frame, so the overlay can keep the gizmo glued to the object while the camera moves.
local camPC, camPCAt, camPending, camSeq = nil, -1000, false, 0
function E.WriteCam()
    camPending = false
    if not selected then return end
    if not Valid(camPC) or tick - camPCAt > 60 then camPC, camPCAt = ViewPC(), tick end
    local vpc = camPC
    if not Valid(vpc) then return end
    local pcm = vpc.PlayerCameraManager
    if not Valid(pcm) then return end
    local l, r, fov = pcm:GetCameraLocation(), pcm:GetCameraRotation(), pcm:GetFOVAngle()
    local size = {}
    vpc:GetViewportSize(size, size)
    camSeq = camSeq + 1
    WriteFile(Dir .. "cam.txt", string.format("%d %.4f %.3f %.3f %.3f %.4f %.4f %.4f %.4f %d %d\n", camSeq, os.clock(),
        l.X, l.Y, l.Z, r.Pitch, r.Yaw, r.Roll, fov, tonumber(size.SizeX) or 0, tonumber(size.SizeY) or 0))
end

-- Everything the editor does every frame, in ONE game-thread job (UE4SS runs only one queued job per frame,
-- so several separate ones would wait in line and lag behind). Called from main.lua.
function E.Busy()
    return E.open or E.watch or (E.PossessActive and E.PossessActive()) or (E.ViewMode and E.ViewMode() ~= "off")
        or (Lay and (Lay.dirty or os.clock() - (Lay.lastClock or -100) > 1)) or E.ModelsBusy()
end
function E.Frame()
    tick = tick + 1
    if Lay then pcall(Lay.Frame, tick) end
    if E.PossessActive and E.PossessActive() and not E.possByHook then pcall(E.PossessFrame) end
    if not E.viewByHook then pcall(E.ViewFrame) end
    pcall(E.ModelsFrame)
    if not E.open then
        if E.watch and tick % 12 == 0 then pcall(E.WriteState) end      -- launcher's Player page (~5x a second)
        return
    end
    if (selected and tick % 3 == 0) or tick % 12 == 0 then pcall(E.WriteState) end
    if tick % 60 == 0 and selected then pcall(E.WriteProps) end
    if tick % 2 == 0 then E.ThumbFrame() end
    if selected then pcall(E.WriteCam) end
end

-- ---------------------------------------------------------------- saved layouts
-- Everything you change in a level with the editor (moved / rotated / scaled / hidden / deleted objects, and
-- models you added) is saved per game and level, and put back the next time that level loads.
Lay = { on = true, map = nil, file = nil, actors = {}, added = {}, byPath = {}, nextId = 1, dirty = false,
        checkAt = nil, applied = {}, addedActor = {}, world = nil, orig = {} }
local ModDir = (function()
    local src = (debug.getinfo(1, "S") or {}).source or ""
    src = src:gsub("^@", "")
    local d = src:match("^(.*)[/\\][Ss]cripts[/\\][^/\\]+$")
    return d and (d .. "\\") or nil
end)()
local function Safe(x) return (tostring(x or ""):gsub("[^%w%-_]", "_")) end
local function GameName()
    local ok, n = pcall(function() return UEHelpers.GetKismetSystemLibrary():GetGameName():ToString() end)
    return (ok and n and n ~= "") and n or "game"
end
local function MapNow()
    local ok, full = pcall(function() return World():GetFullName() end)
    if not ok or not full then return nil end
    return (tostring(full):match("%s(/[^%.%s]+)")) or nil
end
local function F3(v) return string.format("%.3f,%.3f,%.3f", v.X or v.Pitch or 0, v.Y or v.Yaw or 0, v.Z or v.Roll or 0) end
local function R3(r) return string.format("%.3f,%.3f,%.3f", r.Pitch or 0, r.Yaw or 0, r.Roll or 0) end
local function XformOf(a)
    local l, r, sc
    pcall(function() l = a:K2_GetActorLocation() end)
    pcall(function() r = a:K2_GetActorRotation() end)
    pcall(function() sc = a:GetActorScale3D() end)
    if not (l and r) then return nil end
    return { l = { X = l.X, Y = l.Y, Z = l.Z }, r = { Pitch = r.Pitch, Yaw = r.Yaw, Roll = r.Roll }, s = sc and { X = sc.X, Y = sc.Y, Z = sc.Z } or { X = 1, Y = 1, Z = 1 } }
end
function E.LayoutLine() return "layout=" .. Lay.Count() .. "\t" .. (Lay.on and "1" or "0") .. "\t" .. (Lay.gameSafe or "") .. "\t" .. Clean(Lay.map or "") end
function Lay.Count() local n = 0; for _ in pairs(Lay.actors) do n = n + 1 end; for _ in pairs(Lay.added) do n = n + 1 end; return n end
function Lay.Save()
    Lay.dirty = false
    if not Lay.file then return end
    local lines = { "# Saved editor layout - " .. GameName() .. " " .. (Lay.map or "") }
    for path, e in pairs(Lay.actors) do
        if e.deleted then table.insert(lines, "D\t" .. path)
        elseif e.x then table.insert(lines, string.format("A\t%s\t%s\t%s\t%s\t%d", path, F3(e.x.l), R3(e.x.r), F3(e.x.s), e.hidden and 1 or 0))
        elseif e.hidden ~= nil then table.insert(lines, string.format("H\t%s\t%d", path, e.hidden and 1 or 0)) end
        if e.mats and not e.deleted then for k, v in pairs(e.mats) do table.insert(lines, "P\t" .. path .. "\t" .. k .. "\t" .. v) end end
    end
    for _, e in pairs(Lay.added) do
        if e.x then
            table.insert(lines, string.format("%s\t%s\t%s\t%s\t%s\t%d", e.model and "M" or "N", e.model or e.mesh, F3(e.x.l), R3(e.x.r), F3(e.x.s), e.hidden and 1 or 0))
            if e.mats then for k, v in pairs(e.mats) do table.insert(lines, "p\t" .. k .. "\t" .. v) end end   -- (material changes of the model above)
        end
    end
    if #lines == 1 then os.remove(Lay.file); return end
    WriteFile(Lay.file, table.concat(lines, "\n") .. "\n")
end
local function Touch() if Lay.on then Lay.dirty = true end end
local function N3(t) local a, b, c = (t or ""):match("^([%-%d%.eE]+),([%-%d%.eE]+),([%-%d%.eE]+)$"); return tonumber(a), tonumber(b), tonumber(c) end
function Lay.Load(map)
    Lay.map, Lay.actors, Lay.added, Lay.byPath, Lay.applied, Lay.addedActor, Lay.dirty = map, {}, {}, {}, {}, {}, false
    Lay.checkAt = os.clock() + 2
    Lay.gameSafe = Safe(GameName())
    Lay.file = (ModDir and map) and (ModDir .. "layout_" .. Lay.gameSafe .. "__" .. Safe(map) .. ".txt") or nil
    if not Lay.file then return end
    local f = io.open(Lay.file, "r"); if not f then return end
    for line in f:lines() do
        local t = {}; for x in (line .. "\t"):gmatch("([^\t]*)\t") do table.insert(t, x) end
        local function X(i) local a, b, c = N3(t[i]); local p, y, r = N3(t[i + 1]); local sx, sy, sz = N3(t[i + 2])
            if not (a and p and sx) then return nil end
            return { l = { X = a, Y = b, Z = c }, r = { Pitch = p, Yaw = y, Roll = r }, s = { X = sx, Y = sy, Z = sz } } end
        if t[1] == "A" and t[2] then Lay.actors[t[2]] = { x = X(3), hidden = t[6] == "1" }
        elseif t[1] == "H" and t[2] then Lay.actors[t[2]] = { hidden = t[3] == "1" }
        elseif t[1] == "D" and t[2] then Lay.actors[t[2]] = { deleted = true }
        elseif t[1] == "N" and t[2] then
            local id = Lay.nextId; Lay.nextId = id + 1
            Lay.added[id] = { mesh = t[2], x = X(3), hidden = t[6] == "1" }
        elseif t[1] == "M" and t[2] then   -- an imported model (.uemodel file)
            local id = Lay.nextId; Lay.nextId = id + 1
            Lay.added[id] = { model = t[2], x = X(3), hidden = t[6] == "1" }
        elseif t[1] == "P" and t[2] and t[3] then   -- a material change on an object of the level
            local e = Lay.actors[t[2]] or {}; Lay.actors[t[2]] = e
            e.mats = e.mats or {}; e.mats[t[3]] = t[4] or ""
        elseif t[1] == "p" and t[2] then             -- a material change on the model added just above
            local e = Lay.added[Lay.nextId - 1]
            if e then e.mats = e.mats or {}; e.mats[t[2]] = t[3] or "" end
        end
    end
    f:close()
end
-- Put the saved changes back. This runs every couple of seconds for as long as you're in the level, so it also
-- catches parts of the level that stream in later (big levels load in pieces as you move around), parts that
-- unload and load again, and the game reloading a checkpoint or save. Each object is only set once per time it
-- loads (we remember which copy of it we already moved), so it never fights the editor.
local function Addr(o) local ok, v = pcall(function() return o:GetAddress() end); return ok and v or nil end
local function FreshActor(path)
    local ok, o = pcall(function() return StaticFindObject(path) end)
    if ok and Valid(o) then return ToActor(o) end
    return nil
end
local function ApplyOne(a, e)
    if e.deleted then pcall(function() a:K2_DestroyActor() end); return end
    if e.x then
        MakeMovable(a)
        local hit = {}
        if not pcall(function() a:K2_SetActorLocationAndRotation(e.x.l, e.x.r, false, hit, true) end) then pcall(function() a:K2_TeleportTo(e.x.l, e.x.r) end) end
        pcall(function() a:SetActorScale3D(e.x.s) end)
    end
    if e.hidden ~= nil then pcall(function() a:SetActorHiddenInGame(e.hidden) end) end
    if e.mats and E.ApplyMats then pcall(E.ApplyMats, a, e.mats) end
end
function Lay.Apply(force)
    local n = 0
    -- Looking up an object that isn't there (its part of the level isn't loaded, or it had a name the game
    -- makes up each run) can take tens of milliseconds in a big game, so missing ones are looked for again
    -- less and less often (4 s, 8 s, 16 s ... then once a minute), and only a couple of them at a time.
    local now, missBudget = os.clock(), 2
    for path, e in pairs(Lay.actors) do
        local a = nil
        if force or not e.missUntil or now >= e.missUntil then
            if not e.misses or e.misses == 0 or missBudget > 0 or force then
                local t0 = os.clock()
                a = FreshActor(path)
                local ms = (os.clock() - t0) * 1000
                if ms > (E.lookMs or 0) then E.lookMs = ms end
                if a then e.misses, e.missUntil = 0, nil
                else
                    missBudget = missBudget - 1
                    e.misses = (e.misses or 0) + 1
                    -- after 5 misses, wait until the game loads another part of the level (see Lay.Frame)
                    e.missUntil = e.misses >= 5 and math.huge or (now + 2 ^ (e.misses + 1))
                end
            end
        end
        if a then
            local ad = Addr(a)
            if force or ad == nil or ad ~= Lay.applied[path] then
                Lay.applied[path] = ad
                if not Lay.orig[path] then
                    local x = XformOf(a)
                    local okH, h = pcall(function() return a.bHidden end)
                    if x then Lay.orig[path] = { x = x, hidden = (okH and h) and true or false } end
                end
                pcall(ApplyOne, a, e)
                actorCache[path] = nil
                n = n + 1
            end
        end
    end
    -- models you added: (re)make any that aren't in the world right now.
    -- Loading or building one can stall the game for a moment, so one that can't be made is tried a few times
    -- with growing gaps and then left alone, and one that keeps disappearing (the game unloading that area)
    -- is only remade a few times.
    local now = os.clock()
    for id, e in pairs(Lay.added) do
        local cur = Lay.addedActor[id]
        local missing = e.x and not (cur and Valid(cur)) and not e.failed
        if missing and e.retryAt and now < e.retryAt then missing = false end
        if missing and (e.made or 0) >= 4 then e.failed = true; missing = false end
        if missing then
            e.tries = (e.tries or 0) + 1
            e.retryAt = now + math.min(60, 2 ^ (e.tries + 1))      -- 4 s, 8 s, 16 s, ... up to a minute
            if e.tries > 5 then e.failed = true; missing = false end
        end
        if not missing then
            -- nothing to do for this one
        elseif e.model then
            local a = E.BuildModel(e.model, e.x.l, e.x.r, e.x.s)
            if a then
                local path = ObjectPath(a)
                Lay.addedActor[id] = a
                Lay.byPath[path] = id
                e.made = (e.made or 0) + 1; e.tries = 0; e.retryAt = nil
                if e.hidden then pcall(function() a:SetActorHiddenInGame(true) end) end
                if e.mats and E.ApplyMats then pcall(E.ApplyMats, a, e.mats) end
                table.insert(spawned, { path = path, name = (e.model:match("([^\\/]+)%.uemodel$")) or "model" })
                n = n + 1
            else e.failed = true end   -- (file missing on this PC, or the game can't build models)
        elseif e.mesh then
            local mesh = FindMesh(e.mesh, e.loadTried)
            e.loadTried = true            -- only ever ask the game to load it from its files once
            if mesh then
                e.made = (e.made or 0) + 1; e.tries = 0; e.retryAt = nil
                local a = SpawnMeshActor(mesh, e.x.l, e.x.r, e.x.s)
                if a then
                    local path = ObjectPath(a)
                    Lay.addedActor[id] = a
                    Lay.byPath[path] = id
                    if e.hidden then pcall(function() a:SetActorHiddenInGame(true) end) end
                    if e.mats and E.ApplyMats then pcall(E.ApplyMats, a, e.mats) end
                    table.insert(spawned, { path = path, name = (e.mesh:match("([^/%.]+)$")) or "model" })
                    n = n + 1
                end
            end
        end
    end
    return n
end
function Lay.Frame(t)
    if os.clock() - (Lay.lastClock or -100) > 1 then          -- about once a second: has the level changed?
        Lay.lastClock = os.clock()
        local m = MapNow()
        local okW, w = pcall(World)
        local wa = okW and w and Addr(w) or nil
        if m and m ~= Lay.map then
            if Lay.dirty then pcall(Lay.Save) end
            if Lay.map and E.ForgetLevelObjects then pcall(E.ForgetLevelObjects) end
            Lay.Load(m); Lay.world = wa; Lay.orig = {}
        elseif m and wa and Lay.world and wa ~= Lay.world then
            -- same level loaded again (checkpoint / loading a save): everything is fresh, put it all back
            if Lay.dirty then pcall(Lay.Save) end
            if E.ForgetLevelObjects then pcall(E.ForgetLevelObjects) end
            Lay.applied, Lay.addedActor, Lay.byPath, Lay.orig = {}, {}, {}, {}
            for _, e in pairs(Lay.added) do e.made, e.tries, e.retryAt, e.failed, e.loadTried = 0, 0, nil, nil, nil end
            for _, e in pairs(Lay.actors) do e.misses, e.missUntil = 0, nil end
            Lay.levelCount = nil
            Lay.world = wa; Lay.checkAt = os.clock() + 2
        elseif wa and not Lay.world then Lay.world = wa end
        -- another part of the level streamed in or out: look again for saved changes that weren't there
        if okW and w then
            local okL, nl = pcall(function() return #w.Levels end)
            if not okL then okL, nl = pcall(function() return w.Levels:GetArrayNum() end) end
            if okL and nl and nl ~= Lay.levelCount then
                if Lay.levelCount then for _, e in pairs(Lay.actors) do if (e.misses or 0) > 0 then e.misses, e.missUntil = 1, os.clock() + 1 end end end
                Lay.levelCount = nl
            end
        end
        if Lay.dirty then pcall(Lay.Save) end
    end
    if Lay.checkAt and os.clock() >= Lay.checkAt then
        Lay.checkAt = os.clock() + 2
        if Lay.Count() > 0 then
            local t0 = os.clock()
            local okA, n = pcall(Lay.Apply, false)
            E.layMs = math.max(E.layMs or 0, (os.clock() - t0) * 1000)
            if okA and n and n > 0 then Msg(string.format("Put back %d saved change%s in this level", n, n == 1 and "" or "s")) end
        end
    end
end
function Lay.Record(path)
    if not Lay.on or not path or path == "" then return end
    local a = FindActor(path); if not a then return end
    local x = XformOf(a); if not x then return end
    local id = Lay.byPath[path]
    if id and Lay.added[id] then Lay.added[id].x = x
    else
        local e = Lay.actors[path] or {}
        e.x, e.deleted = x, nil
        Lay.applied[path] = Addr(a)
        local okH, h = pcall(function() return a.bHidden end)
        if okH and h ~= nil then e.hidden = h and true or false end
        Lay.actors[path] = e
    end
    Touch()
end
function Lay.Hidden(path, hidden)
    if not Lay.on or not path then return end
    local id = Lay.byPath[path]
    if id and Lay.added[id] then Lay.added[id].hidden = hidden
    else
        local e = Lay.actors[path] or {}; e.hidden = hidden; Lay.actors[path] = e
        local a = FreshActor(path); if a then Lay.applied[path] = Addr(a) end
    end
    Touch()
end
function Lay.Deleted(path)
    if not Lay.on or not path then return end
    local id = Lay.byPath[path]
    if id then Lay.added[id] = nil; Lay.addedActor[id] = nil; Lay.byPath[path] = nil
    else
        Lay.actors[path] = { deleted = true }
        local a = FreshActor(path); if a then Lay.applied[path] = Addr(a) end   -- (kept for Undo: don't delete it for real now)
    end
    Touch()
end
function E.LayModelAdded(a, file)
    if not Lay.on then return end
    local path = ObjectPath(a)
    local id = Lay.nextId; Lay.nextId = id + 1
    Lay.added[id] = { model = file, x = XformOf(a), hidden = false }
    Lay.addedActor[id] = a
    Lay.byPath[path] = id
    Touch()
end
function Lay.AddedNew(a, mesh)
    if not Lay.on then return end
    local path = ObjectPath(a)
    local id = Lay.nextId; Lay.nextId = id + 1
    Lay.added[id] = { mesh = ObjectPath(mesh), x = XformOf(a), hidden = false }
    Lay.addedActor[id] = a
    Lay.byPath[path] = id
    Touch()
end
-- record after the editor changes something
local function After(name, fn)
    local f = handlers[name]; if not f then return end
    handlers[name] = function(arg) local r = f(arg); pcall(fn, arg); return r end
end
local function FirstField(arg) return (arg or ""):match("^([^\t]+)") end
After("ed_xform", function(arg) Lay.Record(FirstField(arg)) end)
After("ed_rot", function(arg) Lay.Record(FirstField(arg)) end)
After("ed_snap", function(arg) Lay.Record(FirstField(arg)) end)
After("ed_bring", function(arg) Lay.Record(FirstField(arg)) end)
After("ed_hide", function(arg) local p, v = (arg or ""):match("^([^\t]+)\t(%d)$"); if p then Lay.Hidden(p, v == "1") end end)
After("ed_destroy", function(arg) Lay.Deleted(arg) end)
handlers.ed_layout = function(arg)
    if arg == "on" then Lay.on = true; Msg("Saving your changes to this level")
    elseif arg == "off" then if Lay.dirty then Lay.Save() end; Lay.on = false; Msg("Not saving changes (what's saved stays saved)")
    elseif arg == "save" then Lay.Save(); Msg(string.format("Saved %d change%s for this level", Lay.Count(), Lay.Count() == 1 and "" or "s"))
    elseif arg == "reset" then
        Lay.actors, Lay.added, Lay.byPath, Lay.applied, Lay.addedActor, Lay.dirty = {}, {}, {}, {}, {}, false
        if Lay.file then os.remove(Lay.file) end
        Msg("Saved changes for this level cleared. Reload the level to see it as the game made it.")
    elseif arg:sub(1, 7) == "import\t" then
        -- a shared layout for this level, from the launcher: replaces what's saved here
        local src = arg:sub(8)
        local f = io.open(src, "r"); if not f then Msg("Couldn't read the shared layout.", true); return end
        local data = f:read("a"); f:close(); os.remove(src)
        if not Lay.file then Msg("No level is loaded yet.", true); return end
        if Lay.dirty then pcall(Lay.Save) end
        local bak = Lay.file .. ".before-import"
        local hb = io.open(bak, "r")
        if hb then hb:close()
        else   -- first shared layout on this level: keep what was saved here (or remember there was nothing)
            local cur = io.open(Lay.file, "r")
            local old = cur and cur:read("a") or ""
            if cur then cur:close() end
            WriteFile(bak, old)
        end
        for _, a in pairs(Lay.addedActor or {}) do if Valid(a) then pcall(function() a:K2_DestroyActor() end) end end   -- models from the old layout
        WriteFile(Lay.file, data)
        local m = Lay.map
        Lay.dirty = false
        Lay.Load(m)
        Lay.checkAt = os.clock()
        Msg(string.format("Loaded a shared layout: %d change%s for this level. Reload the level to see it exactly as shared.", Lay.Count(), Lay.Count() == 1 and "" or "s"))
    elseif arg == "unimport" then
        if not Lay.file then Msg("No level is loaded yet.", true); return end
        -- put objects back how they were before, take away models the shared layout added
        local back = 0
        for path, e in pairs(Lay.actors) do
            local o = Lay.orig[path]
            local a = o and FreshActor(path)
            if a then
                local hit = {}
                if not pcall(function() a:K2_SetActorLocationAndRotation(o.x.l, o.x.r, false, hit, true) end) then pcall(function() a:K2_TeleportTo(o.x.l, o.x.r) end) end
                pcall(function() a:SetActorScale3D(o.x.s) end)
                pcall(function() a:SetActorHiddenInGame(o.hidden) end)
                back = back + 1
            end
        end
        for _, a in pairs(Lay.addedActor or {}) do if Valid(a) then pcall(function() a:K2_DestroyActor() end) end end
        local bak = Lay.file .. ".before-import"
        local hb = io.open(bak, "r")
        local old = hb and hb:read("a") or ""
        if hb then hb:close(); os.remove(bak) end
        if old ~= "" then WriteFile(Lay.file, old) else os.remove(Lay.file) end
        Lay.dirty = false
        local m = Lay.map
        local keep = Lay.orig
        Lay.Load(m); Lay.orig = keep
        local n = Lay.Apply(true)     -- your own saved changes (from before the shared layout), straight away
        Msg("Unloaded the shared layout from this level" .. (n > 0 and string.format(" (your own %d saved change%s %s back)", n, n == 1 and "" or "s", n == 1 and "is" or "are") or "") ..
            ". Anything it deleted comes back when you reload the level.")
    elseif arg == "reapply" then local n = Lay.Apply(true); Msg(string.format("Put back %d saved change%s", n, n == 1 and "" or "s")) end
end

-- ---------------------------------------------------------------- undo
-- Every edit (move / rotate / scale / snap / bring here / hide / delete / add) is remembered so it can be
-- undone, newest first. A drag sends many small moves; they count as one step.
local Undo = { stack = {}, max = 100, lastPath = nil, lastAt = -100 }
local function Snapshot(a)
    local x = XformOf(a); if not x then return nil end
    local okH, h = pcall(function() return a.bHidden end)
    return { x = x, hidden = (okH and h) and true or false }
end
local function Push(e)
    table.insert(Undo.stack, e)
    if #Undo.stack > Undo.max then table.remove(Undo.stack, 1) end
end
local function NameOf(path) return (tostring(path):match("([^%.:]+)$")) or "object" end
local function MoveStart(path, grouped)
    if not path or path == "" then return end
    local now = os.clock()
    if grouped and Undo.lastPath == path and now - Undo.lastAt < 0.8 then Undo.lastAt = now; return end   -- same drag
    local a = FindActor(path); if not a then return end
    local snap = Snapshot(a); if not snap then return end
    Push({ kind = "move", path = path, s = snap, what = "move " .. NameOf(path) })
    Undo.lastPath, Undo.lastAt = grouped and path or nil, now
end
local function Before(name, fn)
    local f = handlers[name]; if not f then return end
    handlers[name] = function(arg) pcall(fn, arg); return f(arg) end
end
Before("ed_xform", function(arg) MoveStart(FirstField(arg), true) end)
Before("ed_rot", function(arg) MoveStart(FirstField(arg), true) end)
Before("ed_snap", function(arg) MoveStart(FirstField(arg), false) end)
Before("ed_bring", function(arg) MoveStart(FirstField(arg), false) end)
Before("ed_hide", function(arg)
    local p = (arg or ""):match("^([^\t]+)\t%d$"); if not p then return end
    local a = FindActor(p); if not a then return end
    local okH, h = pcall(function() return a.bHidden end)
    Push({ kind = "hide", path = p, was = (okH and h) and true or false, what = "hide / show " .. NameOf(p) })
    Undo.lastPath = nil
end)
function E.SoftDelete(path, a)
    local snap = Snapshot(a) or {}
    local okC, col = pcall(function() return a:GetActorEnableCollision() end)
    local e = { kind = "delete", path = path, s = snap, col = not okC or col ~= false, what = "delete " .. NameOf(path) }
    local id = Lay and Lay.byPath[path]
    if id and Lay.added[id] then e.layId, e.layEntry = id, Lay.added[id] end
    for _, sp in ipairs(spawned) do if sp.path == path then e.spawnedName = sp.name end end
    pcall(function() a:SetActorHiddenInGame(true) end)
    pcall(function() a:SetActorEnableCollision(false) end)
    E.softDeleted[path] = true
    Push(e); Undo.lastPath = nil
end
function E.UndoAdded(path)
    Push({ kind = "add", path = path, what = "add " .. NameOf(path) }); Undo.lastPath = nil
end
function E.UndoLine()
    local top = Undo.stack[#Undo.stack]
    return "undo=" .. #Undo.stack .. "\t" .. Clean(top and top.what or "")
end
local function PutBack(a, x)
    MakeMovable(a)
    local hit = {}
    if not pcall(function() a:K2_SetActorLocationAndRotation(x.l, x.r, false, hit, true) end) then pcall(function() a:K2_TeleportTo(x.l, x.r) end) end
    pcall(function() a:SetActorScale3D(x.s) end)
end
local function FindAny(path)
    local a = FindActor(path); if a then return a end
    local ok, o = pcall(function() return StaticFindObject(path) end)
    if ok and Valid(o) then return ToActor(o) end
end
handlers.ed_undo = function()
    local e = table.remove(Undo.stack)
    Undo.lastPath = nil
    if not e then Msg("Nothing to undo", true); return end
    local a = FindAny(e.path)
    if e.kind == "move" then
        if not a then Msg("Can't undo: " .. NameOf(e.path) .. " isn't there any more", true); return end
        PutBack(a, e.s.x)
        if Lay then pcall(Lay.Record, e.path) end
        Msg("Undid: " .. e.what)
    elseif e.kind == "hide" then
        if not a then Msg("Can't undo: " .. NameOf(e.path) .. " isn't there any more", true); return end
        pcall(function() a:SetActorHiddenInGame(e.was) end)
        if Lay then pcall(Lay.Hidden, e.path, e.was) end
        Msg("Undid: " .. e.what)
    elseif e.kind == "delete" then
        if not a then Msg("Can't undo: the game removed " .. NameOf(e.path), true); E.softDeleted[e.path] = nil; return end
        E.softDeleted[e.path] = nil
        pcall(function() a:SetActorHiddenInGame(e.s.hidden or false) end)
        pcall(function() a:SetActorEnableCollision(e.col) end)
        if e.s.x then PutBack(a, e.s.x) end
        if Lay then
            if e.layId then
                Lay.added[e.layId], Lay.addedActor[e.layId], Lay.byPath[e.path] = e.layEntry, a, e.layId
                Lay.dirty = Lay.on
            else pcall(Lay.Record, e.path) end
        end
        if e.spawnedName then table.insert(spawned, { path = e.path, name = e.spawnedName }) end
        actorCache[e.path] = a
        E.Select(e.path, a)
        Msg("Undid: " .. e.what)
    elseif e.kind == "add" then
        if a then pcall(function() a:K2_DestroyActor() end) end
        if Lay then pcall(Lay.Deleted, e.path) end
        actorCache[e.path] = nil
        if selectedPath == e.path then selected, selectedPath = nil, "" end
        Msg("Undid: " .. e.what)
    end
    pcall(E.WriteProps)
end

-- ---------------------------------------------------------------- material editor
-- The launcher's material editor shows each material an object uses as a graph: the material's settings
-- (pictures, colours, numbers) are the inputs of its output node, fed by nodes you can change or re-wire.
-- Shipped games can't build new materials (that needs Unreal's shader compiler), so what can be changed
-- is what the material itself exposes: its parameters, or the whole material swapped for another one.
-- Changes go onto a copy of the material made for that object only (a dynamic material instance), and are
-- saved with the level's layout like any other edit.
(function()
local MI = { origs = {}, mids = {}, snaps = {}, prevSeq = 0, edits = {} }
local function Arr(t)
    local out = {}
    if t == nil then return out end
    local ok = pcall(function() t:ForEach(function(_, e) table.insert(out, e:get()) end) end)
    if not ok or #out == 0 then
        pcall(function() local n = #t; for i = 1, n do table.insert(out, t[i]) end end)
    end
    return out
end
local function PName(v)
    local n = nil
    pcall(function() n = v.ParameterInfo.Name:ToString() end)
    if not n then pcall(function() n = v.ParameterName:ToString() end) end
    return n
end
local function Col(c) return string.format("%.4f,%.4f,%.4f,%.4f", c.R or 0, c.G or 0, c.B or 0, c.A or 1) end
local isaCls = {}
local function IsA(o, cls)
    if type(cls) == "string" then
        if isaCls[cls] == nil then local c = nil; pcall(function() c = StaticFindObject(cls) end); isaCls[cls] = Valid(c) and c or false end
        if not isaCls[cls] then return false end
        cls = isaCls[cls]
    end
    local ok, r = pcall(function() return o:IsA(cls) end)
    return ok and r == true
end
E.MatParams = function(m) return MI.Params(m) end
-- the parameters a material (instance) has, the most specific value first:
-- { tex = { {name, value} }, vec = ..., sca = ... }
function MI.Params(mat)
    local P = { tex = {}, vec = {}, sca = {}, seen = {} }
    -- the base material's own list (Unreal 4.26 and newer): a copy can carry settings the material doesn't have
    -- (an imported model's stand-in gets every common name set to a neutral value); those are left out
    local real, base = nil, nil
    pcall(function() base = mat:GetBaseMaterial() end)
    if Valid(base) then
        local B = {}
        pcall(MI.BaseParams, base, function(kind, name) if name and name ~= "None" and name ~= "" then B[kind .. "|" .. name] = true end end)
        if next(B) then real = B end
    end
    -- the base material's list unreadable: on a copy, leave out the names the importer sets on every copy
    -- unless a material under the copy really has them
    local under, atCopy = nil, false
    if not real and IsA(mat, "/Script/Engine.MaterialInstanceDynamic") then
        under = {}
        local cur, d = nil, 0
        pcall(function() cur = mat.Parent end)
        while Valid(cur) and d < 8 do
            for _, f in ipairs({ { "TextureParameterValues", "tex" }, { "VectorParameterValues", "vec" }, { "ScalarParameterValues", "sca" } }) do
                pcall(function() for _, v in ipairs(Arr(cur[f[1]])) do under[f[2] .. "|" .. PName(v)] = true end end)
            end
            local par = nil; pcall(function() par = cur.Parent end)
            cur, d = par, d + 1
        end
    end
    local function add(kind, name, val)
        if not name or name == "None" or name == "" then return end
        local k = kind .. "|" .. name
        if P.seen[k] then return end
        -- (the list read from the base material can be incomplete (only its numbers, on some engine versions), so it
        -- only rules out the names the importer sets on every copy)
        if real and not real[k] and E.NEUTRAL_NAMES[name] then return end
        if atCopy and under and not under[k] and E.NEUTRAL_NAMES[name] then return end
        P.seen[k] = true
        table.insert(P[kind], { name = name, value = val })
    end
    local cur, depth = mat, 0
    while Valid(cur) and depth < 8 do
        atCopy = depth == 0
        pcall(function() for _, v in ipairs(Arr(cur.TextureParameterValues)) do
            local t = nil; pcall(function() t = v.ParameterValue end)
            add("tex", PName(v), Valid(t) and ObjectPath(t) or "")
        end end)
        pcall(function() for _, v in ipairs(Arr(cur.VectorParameterValues)) do
            local c = nil; pcall(function() c = v.ParameterValue end)
            if c then add("vec", PName(v), Col(c)) end
        end end)
        pcall(function() for _, v in ipairs(Arr(cur.ScalarParameterValues)) do
            local s = nil; pcall(function() s = v.ParameterValue end)
            if type(s) == "number" then add("sca", PName(v), string.format("%.4f", s)) end
        end end)
        local par = nil; pcall(function() par = cur.Parent end)
        if not Valid(par) then break end
        cur, depth = par, depth + 1
    end
    -- the base material's own parameters (kept with the cooked material, Unreal 4.26 and newer). Some master
    -- materials have hundreds (Hogwarts Legacy's: over a thousand): then only the ones that matter for the look
    -- (textures, colour / tint, glow, roughness, metal) are listed, plus any the instances set (above)
    atCopy = false
    if Valid(base) then
        local list = {}
        pcall(MI.BaseParams, base, function(kind, name, val) table.insert(list, { kind, name, val }) end)
        local many = #list > 80
        for _, e in ipairs(list) do
            local role = E.ParamRole(e[1], e[2])
            if not many or e[1] == "tex" or (e[1] == "vec" and (role == "tint" or role == "emissive")) or (e[1] == "sca" and (role == "rough" or role == "metal")) then add(e[1], e[2], e[3]) end
        end
    end
    return P
end
function MI.BaseParams(base, add)
    local cp = nil
    pcall(function() cp = base.CachedExpressionData.Parameters end)
    if cp == nil then return end
    local entries = {}
    pcall(function() entries = Arr(cp.RuntimeEntries) end)
    if #entries == 0 then pcall(function() entries = { cp.RuntimeEntries } end) end
    local values = { sca = {}, vec = {}, tex = {} }
    pcall(function() values.sca = Arr(cp.ScalarValues) end)
    pcall(function() values.vec = Arr(cp.VectorValues) end)
    pcall(function() values.tex = Arr(cp.TextureValues) end)
    local kinds = { "sca", "vec", "tex" }
    for i, entry in ipairs(entries) do
        local kind = kinds[i]
        if kind then
            local infos = {}
            pcall(function() infos = Arr(entry.ParameterInfos) end)
            for j, info in ipairs(infos) do
                local name = nil; pcall(function() name = info.Name:ToString() end)
                local v = values[kind][j]
                if kind == "sca" then add("sca", name, type(v) == "number" and string.format("%.4f", v) or "0")
                elseif kind == "vec" then add("vec", name, v and Col(v) or "1,1,1,1")
                else add("tex", name, Valid(v) and ObjectPath(v) or "") end
            end
        end
    end
end
function MI.Comps(a)
    local out = {}
    local cls = StaticFindObject("/Script/Engine.MeshComponent")
    if Valid(cls) then
        local ok, arr = pcall(function() return a:K2_GetComponentsByClass(cls) end)
        if ok and arr then for _, c in ipairs(Arr(arr)) do if Valid(c) and IsA(c, "/Script/Engine.MeshComponent") then table.insert(out, c) end end end
    end
    if #out > 0 then return out end
    -- if the engine doesn't hand the list back: walk the object's own parts (what's attached to it, and the parts
    -- its blueprint / the editor made), never the whole game's
    local seen = {}
    local function add(c)
        if not Valid(c) then return end
        local ad = nil; pcall(function() ad = c:GetAddress() end)
        if not ad or seen[ad] then return end
        seen[ad] = true
        if IsA(c, "/Script/Engine.MeshComponent") then table.insert(out, c) end
    end
    local function walk(c, depth)
        if depth > 8 or not Valid(c) then return end
        add(c)
        pcall(function() for _, ch in ipairs(Arr(c.AttachChildren)) do walk(ch, depth + 1) end end)
    end
    pcall(function() walk(a.RootComponent, 0) end)
    pcall(function() for _, c in ipairs(Arr(a.BlueprintCreatedComponents)) do add(c) end end)
    pcall(function() for _, c in ipairs(Arr(a.InstanceComponents)) do add(c) end end)
    return out
end
function MI.CompName(c) local n = "?"; pcall(function() n = c:GetFName():ToString() end); return n end
-- A part's id in saved changes: its name, or "#n" (its place among the object's parts) when the name is one the
-- engine makes up each time (parts added at runtime, like an imported model's, get a new number every load).
function MI.CompId(c, i) local n = MI.CompName(c); if n:match("_%d%d%d%d%d%d+$") or n == "?" then return "#" .. i end; return n end
function MI.FindComp(a, id)
    local list = MI.Comps(a)
    local idx = tostring(id):match("^#(%d+)$")
    if idx then return list[tonumber(idx)] end
    for _, c in ipairs(list) do if MI.CompName(c) == id then return c end end
    return nil
end
function MI.Key(c, slot) local ad = 0; pcall(function() ad = c:GetAddress() end); return tostring(ad) .. "|" .. tostring(slot) end
function MI.Same(a, b) local ok, r = pcall(function() return a:GetAddress() == b:GetAddress() end); return ok and r end

-- matgraph.txt: every material slot of the object and the settings of each
handlers.ed_matread = function(path)
    local a = FindActor(path)
    if not a then WriteFile(Dir .. "matgraph.txt", "for=" .. Clean(path) .. "\nerr=That object isn't there any more.\nend=1\n"); return end
    local lines = { "for=" .. Clean(path), "name=" .. Clean(Str(a:GetFName())) }
    local inst = MI.ModelOf(path)
    if inst then table.insert(lines, "model=" .. Clean(inst.m.name or inst.file)) end
    local n = 0
    for ci, c in ipairs(MI.Comps(a)) do
        local cn = MI.CompId(c, ci)
        local nm = 0; pcall(function() nm = c:GetNumMaterials() end)
        if nm == 2 then pcall(function() if c:IsMeshSectionVisible(1) == false then nm = 1 end end) end   -- (an imported model's hidden collision copy)
        local slotNames = {}
        pcall(function() for _, s in ipairs(Arr(c:GetMaterialSlotNames())) do
            local v = s; pcall(function() v = s:get() end)
            local t = Str(v); if t:find("RemoteUnrealParam", 1, true) or t == "None" then t = "" end
            table.insert(slotNames, t)
        end end)
        for s = 0, nm - 1 do
            if n >= 48 then break end
            local m = nil; pcall(function() m = c:GetMaterial(s) end)
            if Valid(m) then
                n = n + 1
                local orig = MI.origs[MI.Key(c, s)]
                if not Valid(orig) then orig = m end
                -- the material shown on the graph: the one under our copy if we made one
                local shown = m
                if IsA(m, "/Script/Engine.MaterialInstanceDynamic") then local par = nil; pcall(function() par = m.Parent end); if Valid(par) and MI.mids[MI.Key(c, s)] then shown = par end end
                local base = nil; pcall(function() base = m:GetBaseMaterial() end)
                table.insert(lines, string.format("slot=%d\t%s\t%d\t%s\t%s\t%s\t%s", n, Clean(cn), s, Clean(ObjectPath(shown)), Clean(ObjectPath(orig)),
                    Clean(slotNames[s + 1] or ""), Clean(Valid(base) and ObjectPath(base) or "")))
                local cur, org = MI.Params(m), MI.snaps[MI.Key(c, s)] or MI.Params(orig)
                for _, kind in ipairs({ "tex", "vec", "sca" }) do
                    local o = {}; for _, e in ipairs(org[kind]) do o[e.name] = e.value end
                    for _, e in ipairs(cur[kind]) do
                        -- a texture made from a picture file shows as that file (so the launcher can read and preview it)
                        local function shown(v)
                            if kind == "tex" and type(v) == "string" and v:find("/Engine/Transient", 1, true) and E.TextureSource then
                                local src = E.TextureSource(v); if src then return "file:" .. src end
                            end
                            return v
                        end
                        local v = shown(e.value)
                        table.insert(lines, string.format("%s=%d\t%s\t%s\t%s", kind, n, Clean(e.name), Clean(v), Clean(o[e.name] and shown(o[e.name]) or v)))
                    end
                end
            end
        end
    end
    if n == 0 then table.insert(lines, "err=This object has no materials the editor can reach.") end
    table.insert(lines, "end=1")
    WriteFile(Dir .. "matgraph.txt", table.concat(lines, "\n") .. "\n")
end

-- the Material Graph's result: baked pictures put into the material's texture slots by what each slot is for.
-- arg: actor \t component \t slot \t file prefix \t what was made (comma list: bc,n,pack,em,op)
-- files: <prefix>_basecolor / _normal / _orm / _mra / _rma / _rough / _metal / _ao / _emissive / _opacity .png
handlers.ed_matbake = function(arg)
    local t = {}; for x in ((arg or "") .. "\t"):gmatch("([^\t]*)\t") do table.insert(t, x) end
    local path, comp, slot, prefix, made = t[1], t[2], tonumber(t[3]) or 0, t[4], ","..(t[5] or "")..","
    local a = FindActor(path); if not a or not prefix then Msg("That object isn't there any more.", true); return end
    local c = MI.FindComp(a, comp); if not c then Msg("That part of the object isn't there any more.", true); return end
    local m = nil; pcall(function() m = c:GetMaterial(slot) end)
    if not Valid(m) then Msg("That part has no material.", true); return end
    local want = { colour = { "bc", "_basecolor" }, ["colour?"] = { "bc", "_basecolor" }, normal = { "n", "_normal" },
                   orm = { "pack", "_orm" }, mra = { "pack", "_mra" }, rma = { "pack", "_rma" }, rough = { "pack", "_rough" },
                   metal = { "pack", "_metal" }, ao = { "pack", "_ao" }, emissive = { "em", "_emissive" }, opacity = { "op", "_opacity" } }
    local function fill(mat, kinds)
        local done, skipped = {}, {}
        for _, e in ipairs(MI.Params(mat).tex) do
            local kind = kinds and kinds[e.name]
            if not kind then local ok, k = pcall(TexKind, e.name, MI.Texture(e.value)); kind = ok and k or nil end
            local w = kind and want[kind] or nil
            if w and made:find("," .. w[1] .. ",", 1, true) then
                handlers.ed_matset(table.concat({ path, comp, tostring(slot), "tex", e.name, "file:" .. prefix .. w[2] .. ".png" }, "\t"))
                table.insert(done, e.name .. " (" .. kind .. ")")
            elseif not w then table.insert(skipped, e.name) end
        end
        return done, skipped
    end
    local done, skipped = fill(m)
    if #done == 0 then
        -- no slot for any of it (a material whose look is built into its own graph): swap it for a game material that
        -- has colour / normal / roughness slots (the one imported models use), then fill those
        local okT, baseM, _, _, _, slots = pcall(TexturedMaterial, c, 0, false)
        if okT and Valid(baseM) then
            handlers.ed_matset(table.concat({ path, comp, tostring(slot), "mat", "", ObjectPath(baseM) }, "\t"))
            local nm = nil; pcall(function() nm = c:GetMaterial(slot) end)
            if Valid(nm) then
                pcall(Neutralise, nm, slots ~= nil)
                local kinds = nil
                if slots then kinds = {}; for _, sl in ipairs(slots) do kinds[sl.param] = sl.kind end end
                done, skipped = fill(nm, kinds)
                if #done > 0 then table.insert(done, 1, "material swapped for " .. (ObjectPath(baseM):match("([^%.]+)$") or "a game material")) end
            end
        end
    end
    if #done == 0 then Msg("The material has no texture slots the graph's results fit (" .. (#skipped > 0 and table.concat(skipped, ", ") or "none") .. "), and no other game material could take them.", true); return end
    Msg("Material Graph applied: " .. table.concat(done, ", ") .. (#skipped > 0 and ("  (left alone: " .. table.concat(skipped, ", ") .. ")") or ""))
end

-- the material copy (made the first time a setting is changed) for one slot
function MI.Mid(c, slot)
    local key = MI.Key(c, slot)
    local cur = nil; pcall(function() cur = c:GetMaterial(slot) end)
    if not Valid(cur) then return nil end
    if not Valid(MI.origs[key]) then MI.origs[key] = cur end
    -- the settings as they were before the first change (a copy the game hands back may be the same object, so
    -- "put back" can't rely on the old object alone)
    if not MI.snaps[key] then local okP, P = pcall(MI.Params, MI.origs[key]); if okP then MI.snaps[key] = P end end
    local mid = MI.mids[key]
    if Valid(mid) and MI.Same(mid, cur) then return mid end
    -- already a copy (an imported model's parts are): change it directly. A copy of a copy isn't allowed (the engine
    -- then draws its plain grid material instead, and the part loses its textures and glow); "reset" puts the
    -- snapshot above back.
    if IsA(cur, "/Script/Engine.MaterialInstanceDynamic") then MI.mids[key] = cur; return cur end
    -- always a new copy on top of what's there (the part's own CreateDynamicMaterialInstance hands back the material
    -- itself when it's already a copy, like an imported model's, and then "reset" would have nothing to go back to)
    local made = nil
    pcall(function()
        local kml = StaticFindObject("/Script/Engine.Default__KismetMaterialLibrary")
        made = kml:CreateDynamicMaterialInstance(World(), cur, FName(""), 0)
    end)
    if not Valid(made) then
        pcall(function() local kml = StaticFindObject("/Script/Engine.Default__KismetMaterialLibrary"); made = kml:CreateDynamicMaterialInstance(World(), cur, FName("")) end)
    end
    if Valid(made) then pcall(function() c:SetMaterial(slot, made) end)
    else
        local ok, m2 = pcall(function() return c:CreateDynamicMaterialInstance(slot, cur, FName("")) end)
        if not (ok and Valid(m2)) then return nil end
        made = m2
    end
    MI.mids[key] = made
    return made
end
function MI.Texture(value)
    if not value or value == "" then return nil end
    if value:sub(1, 5) == "file:" then return LoadTextureFile(value:sub(6)) end
    local t = nil
    pcall(function() t = StaticFindObject(value) end)
    if not Valid(t) then t = LoadAsset(value) end
    return Valid(t) and t or nil
end
-- one change: kind tex / vec / sca (a setting), mat (swap the whole material; "" = the original), reset (back to the original)
function MI.Apply(a, comp, slot, kind, param, value)
    local c = MI.FindComp(a, comp); if not c then return false, "That part of the object isn't there any more." end
    slot = tonumber(slot) or 0
    local key = MI.Key(c, slot)
    local cur = nil; pcall(function() cur = c:GetMaterial(slot) end)
    if not Valid(MI.origs[key]) and Valid(cur) then MI.origs[key] = cur end
    if kind == "reset" or (kind == "mat" and value == "") then
        MI.Restore(c, slot)
        return true
    elseif kind == "mat" then
        local m = nil
        pcall(function() m = StaticFindObject(value) end)
        if not Valid(m) then m = LoadAsset(value) end
        if not Valid(m) then return false, "The game couldn't load that material." end
        pcall(function() c:SetMaterial(slot, m) end)
        MI.mids[key] = nil
        return true
    end
    local mid = MI.Mid(c, slot)
    if not mid then return false, "The game wouldn't make an editable copy of this material." end
    if kind == "tex" then
        local t = MI.Texture(value)
        if not t then return false, "The game couldn't load that picture." end
        mid:SetTextureParameterValue(FName(param), t)
    elseif kind == "vec" then
        local r, g, b, al = tostring(value):match("^([%-%d%.eE]+),([%-%d%.eE]+),([%-%d%.eE]+),?([%-%d%.eE]*)$")
        if not r then return false, "bad colour" end
        mid:SetVectorParameterValue(FName(param), { R = tonumber(r), G = tonumber(g), B = tonumber(b), A = tonumber(al) or 1 })
    elseif kind == "sca" then
        mid:SetScalarParameterValue(FName(param), tonumber(value) or 0)
    else return false, "unknown change" end
    return true
end
-- a new level: the materials and parts remembered for the old one are gone (never touch them again)
function E.MatsMapChanged() MI.origs, MI.mids, MI.snaps = {}, {}, {} end
-- put saved changes back on an object: mats = { ["comp|slot|kind|param"] = value }
function E.ApplyMats(a, mats)
    -- only on an object of the world that's running now (never on one going away while a level unloads)
    if not Valid(a) then return end
    local okA, aw = pcall(function() return a:GetWorld():GetAddress() end)
    local okW, cw = pcall(function() return World():GetAddress() end)
    if not (okA and okW and aw and aw == cw) then return end
    local okD, dying = pcall(function() return a:IsActorBeingDestroyed() end)
    if okD and dying then return end
    local keys = {}
    for k in pairs(mats) do table.insert(keys, k) end
    -- a swapped material first, then its settings
    table.sort(keys, function(x, y)
        local mx, my = x:find("|mat|", 1, true) ~= nil, y:find("|mat|", 1, true) ~= nil
        if mx ~= my then return mx end
        return x < y
    end)
    for _, k in ipairs(keys) do
        local comp, slot, kind, param = k:match("^(.-)|(%d+)|(%a+)|(.*)$")
        if comp then pcall(MI.Apply, a, comp, slot, kind, param, mats[k]) end
    end
end
-- remember a change in the level's saved layout
function MI.Save(path, comp, slot, kind, param, value)
    if not Lay or not Lay.on then return end
    local id = Lay.byPath[path]
    local e
    if id and Lay.added[id] then e = Lay.added[id]
    else
        e = Lay.actors[path] or {}
        Lay.actors[path] = e
        local fa = FreshActor(path); if fa then Lay.applied[path] = Addr(fa) end
    end
    e.mats = e.mats or {}
    local pre = comp .. "|" .. tostring(slot) .. "|"
    if kind == "reset" or kind == "mat" then
        for k in pairs(e.mats) do if k:sub(1, #pre) == pre then e.mats[k] = nil end end
        if kind == "mat" and value ~= "" then e.mats[pre .. "mat|"] = value end
    else e.mats[pre .. kind .. "|" .. param] = value end
    if next(e.mats) == nil then e.mats = nil end
    Touch()
end
-- ---- material changes kept with an imported model (<model>.uemodel.edits, next to the model)
-- Every copy of the model, in this game or any other, gets them. The material underneath differs between games,
-- so each change is kept with its setting's name and what it does (its role): where the other game's material has
-- the same setting it's used as-is, otherwise by role (a colour tint goes to that material's tint settings, a picture
-- for the colour slot to its colour slot, and so on).
-- lines: edit \t <model material no.> \t <kind> \t <setting> \t <role> \t <value>
function MI.EditsPath(file) return file .. ".edits" end
function MI.LoadEdits(file)
    local list = {}
    local f = io.open(MI.EditsPath(file), "r"); if not f then return list end
    for line in f:lines() do
        local t = SplitTabs(line)
        if t[1] == "edit" and t[2] then table.insert(list, { mat = tonumber(t[2]), kind = t[3] or "", param = t[4] or "", role = t[5] or "", value = t[6] or "" }) end
    end
    f:close()
    return list
end
function MI.SaveEdits(file, list)
    if #list == 0 then os.remove(MI.EditsPath(file)); return end
    local out = { "# Material changes made in the UEPRO material editor; every copy of this model gets them" }
    for _, e in ipairs(list) do table.insert(out, table.concat({ "edit", tostring(e.mat), e.kind, e.param, e.role, e.value }, "\t")) end
    local f = io.open(MI.EditsPath(file), "w"); if f then f:write(table.concat(out, "\n") .. "\n"); f:close() end
end
-- the imported model an object is (its live record), and which of the model's materials a part of it shows
function MI.ModelOf(path) local inst = Models.live[path]; if inst and inst.file and not inst.real then return inst end return nil end
function MI.PartMat(inst, c)
    for i, comp in ipairs(inst.comps or {}) do if Valid(comp) and MI.Same(comp, c) then local p = inst.m.parts[i]; return p and p.mat end end
    return nil
end
function MI.TexRole(param, value)
    local t = MI.Texture(value)
    if not t then return "" end
    local ok, k = pcall(TexKind, param, t)
    return ok and k or ""
end
-- a change made in the material editor on an imported model: keep it with the model too
function MI.ModelEdit(path, a, comp, slot, kind, param, value)
    local inst = MI.ModelOf(path); if not inst then return end
    local c = MI.FindComp(a, comp); if not c then return end
    local pm = MI.PartMat(inst, c); if pm == nil then return end
    local list = MI.LoadEdits(inst.file)
    local keep = {}
    if kind == "reset" or kind == "mat" then
        for _, e in ipairs(list) do if e.mat ~= pm then table.insert(keep, e) end end
        if kind == "mat" and value ~= "" then table.insert(keep, { mat = pm, kind = "mat", param = "", role = "", value = value }) end
    else
        if kind == "tex" and (value == "" or value:find("^/Engine/Transient")) then return end     -- the model's own pictures: nothing to keep
        local role = kind == "tex" and MI.TexRole(param, value) or E.ParamRole(kind, param)
        local done = false
        for _, e in ipairs(list) do
            if e.mat == pm and e.kind == kind and e.param == param then e.value, e.role, done = value, role, true end
            table.insert(keep, e)
        end
        if not done then table.insert(keep, { mat = pm, kind = kind, param = param, role = role, value = value }) end
    end
    MI.SaveEdits(inst.file, keep)
    MI.edits[inst.file] = nil
end
-- put a model's kept changes on a freshly built copy
function E.ApplyModelEdits(a, inst)
    if not (a and inst and inst.file) then return end
    local list = MI.LoadEdits(inst.file)
    if #list == 0 then return end
    for i, c in ipairs(inst.comps or {}) do
        local p = inst.m.parts[i]
        if Valid(c) and p and p.mat ~= nil then
            local mine = {}
            for _, e in ipairs(list) do if e.mat == p.mat then table.insert(mine, e) end end
            if #mine > 0 then pcall(MI.ApplyEditsTo, a, c, mine) end
        end
    end
end
function MI.ApplyEditsTo(a, c, mine)
    -- a swapped material first (only if this game has it)
    for _, e in ipairs(mine) do if e.kind == "mat" then
        local m = nil; pcall(function() m = StaticFindObject(e.value) end)
        if not Valid(m) then m = LoadAsset(e.value) end
        if Valid(m) then pcall(function() c:SetMaterial(0, m) end) end
    end end
    local cur = nil; pcall(function() cur = c:GetMaterial(0) end)
    if not Valid(cur) then return end
    local P = MI.Params(cur)
    local have = { tex = {}, vec = {}, sca = {} }
    for _, k in ipairs({ "tex", "vec", "sca" }) do for _, x in ipairs(P[k]) do have[k][x.name] = x end end
    local mid = nil
    local function M() if not mid then mid = MI.Mid(c, 0) end return mid end
    local tint, tinted = { 1, 1, 1, 1 }, false
    local byRole = { vec = {}, sca = {}, tex = {} }
    for _, e in ipairs(mine) do
        if e.kind ~= "mat" then
            if have[e.kind] and have[e.kind][e.param] then
                pcall(MI.SetOn, M(), e.kind, e.param, e.value)                          -- the same setting exists: use it as-is
            elseif e.role == "tint" and e.kind == "vec" then                              -- tints combine (several can be set)
                local r, g, b = tostring(e.value):match("^([%-%d%.eE]+),([%-%d%.eE]+),([%-%d%.eE]+)")
                if r then tint[1], tint[2], tint[3] = tint[1] * tonumber(r), tint[2] * tonumber(g), tint[3] * tonumber(b); tinted = true end
            elseif e.role ~= "" then byRole[e.kind][e.role] = e.value end
        end
    end
    if tinted then byRole.vec.tint = string.format("%.4f,%.4f,%.4f,1", tint[1], tint[2], tint[3]) end
    -- by role: onto this game's settings that do the same thing
    for kind, roles in pairs(byRole) do for role, value in pairs(roles) do
        for _, x in ipairs(P[kind]) do
            local xr = kind == "tex" and (MI.TexRoleOf and MI.TexRoleOf(x) or "") or E.ParamRole(kind, x.name)
            if xr == role then pcall(MI.SetOn, M(), kind, x.name, value) end
        end
    end end
end
-- back to how a slot was before any change: its original material, with its original settings
function MI.Restore(c, slot)
    local key = MI.Key(c, slot)
    local o = MI.origs[key]
    if Valid(o) then pcall(function() c:SetMaterial(slot, o) end) end
    local P = MI.snaps[key]
    if P and Valid(o) and IsA(o, "/Script/Engine.MaterialInstanceDynamic") then
        for _, k in ipairs({ "vec", "sca", "tex" }) do for _, x in ipairs(P[k]) do
            if not (k == "tex" and x.value == "") then pcall(MI.SetOn, o, k, x.name, x.value) end
        end end
    end
    MI.mids[key], MI.snaps[key] = nil, nil
end
function MI.TexRoleOf(x) local t = MI.Texture(x.value); if not t then return "" end; local ok, k = pcall(TexKind, x.name, t); return ok and k or "" end
function MI.SetOn(mid, kind, param, value)
    if not mid then return end
    if kind == "tex" then local t = MI.Texture(value); if t then mid:SetTextureParameterValue(FName(param), t) end
    elseif kind == "vec" then
        local r, g, b, al = tostring(value):match("^([%-%d%.eE]+),([%-%d%.eE]+),([%-%d%.eE]+),?([%-%d%.eE]*)$")
        if r then mid:SetVectorParameterValue(FName(param), { R = tonumber(r), G = tonumber(g), B = tonumber(b), A = tonumber(al) or 1 }) end
    elseif kind == "sca" then mid:SetScalarParameterValue(FName(param), tonumber(value) or 0) end
end
-- arg: actor \t component \t slot \t kind \t parameter \t value
handlers.ed_matset = function(arg)
    local t = {}; for x in ((arg or "") .. "\t"):gmatch("([^\t]*)\t") do table.insert(t, x) end
    local path, comp, slot, kind, param, value = t[1], t[2], t[3], t[4], t[5] or "", t[6] or ""
    if not (path and comp and slot and kind) then return end
    local a = FindActor(path); if not a then Msg("That object isn't there any more.", true); return end
    local ok, err = MI.Apply(a, comp, slot, kind, param, value)
    if not ok then Msg(err or "Couldn't change the material.", true); return end
    MI.Save(path, comp, slot, kind, param, value)
    pcall(MI.ModelEdit, path, a, comp, slot, kind, param, value)
end
-- reset every material of an object
handlers.ed_matresetall = function(path)
    local a = FindActor(path); if not a then return end
    local n = 0
    for _, c in ipairs(MI.Comps(a)) do
        local nm = 0; pcall(function() nm = c:GetNumMaterials() end)
        for s = 0, nm - 1 do
            local key = MI.Key(c, s)
            if Valid(MI.origs[key]) then MI.Restore(c, s); n = n + 1 end
        end
    end
    if Lay then
        local id = Lay.byPath[path]
        local e = (id and Lay.added[id]) or Lay.actors[path]
        if e and e.mats then e.mats = nil; Touch() end
    end
    local inst = MI.ModelOf(path)
    if inst then pcall(MI.SaveEdits, inst.file, {}) end
    Msg(n > 0 and "Put the original materials back" or "Nothing to reset")
end

-- a picture of the material on a sphere (lit like the model pictures): arg id \t actor \t component \t slot
handlers.ed_matprev = function(arg)
    local id, path, comp, slot = (arg or ""):match("^([%w_]+)\t([^\t]+)\t([^\t]+)\t(%d+)$")
    if not id then return end
    local dir = thumbDir or Dir
    local function fail() WriteFile(dir .. id .. ".fail", "x") end
    local a = FindActor(path); if not a then fail(); return end
    local c = MI.FindComp(a, comp); if not c then fail(); return end
    local mat = nil; pcall(function() mat = c:GetMaterial(tonumber(slot)) end)
    if not Valid(mat) then fail(); return end
    local mesh = nil
    for _, p in ipairs({ "/Engine/BasicShapes/Sphere.Sphere", "/Engine/EngineMeshes/Sphere.Sphere", "/Engine/EditorMeshes/EditorSphere.EditorSphere" }) do
        pcall(function() mesh = StaticFindObject(p) end)
        if not Valid(mesh) then mesh = LoadAsset(p) end
        if Valid(mesh) then break end
    end
    if not Valid(mesh) then fail(); return end
    local st = MakeStudio(); if not st then fail(); return end
    local world, krl = World(), KRL()
    local cx, cy, cz = ViewLocation()
    local base = { X = cx, Y = cy, Z = cz + 60000 }
    local sc = st.mesh.StaticMeshComponent
    sc:SetStaticMesh(mesh)
    pcall(function() sc:SetMaterial(0, mat) end)
    local hit = {}
    st.mesh:K2_SetActorLocationAndRotation(base, Rot(0, 0, 0), false, hit, true)
    local cxm, cym, czm, r = base.X, base.Y, base.Z, 50
    pcall(function()
        local b = mesh:GetBounds()
        if b and b.Origin and b.SphereRadius then cxm, cym, czm = base.X + b.Origin.X, base.Y + b.Origin.Y, base.Z + b.Origin.Z; r = math.max(1, b.SphereRadius) end
    end)
    local dist = r / math.sin(math.rad(15)) * 1.05
    local yaw, pitch = math.rad(180), math.rad(-10)
    local dx, dy, dz = math.cos(pitch) * math.cos(yaw), math.cos(pitch) * math.sin(yaw), math.sin(pitch)
    local camLoc = { X = cxm - dx * dist, Y = cym - dy * dist, Z = czm - dz * dist }
    st.cam:K2_SetActorLocationAndRotation(camLoc, Rot(math.deg(pitch), math.deg(yaw), 0), false, hit, true)
    local spots = { { side = 0.7, up = 0.9, k = 1.0 }, { side = -0.9, up = 0.2, k = 0.35 } }
    local dm = dist / 100
    for i, l in ipairs(st.lights or {}) do
        local sp = spots[i]
        local rx, ry = -dy, dx
        local ll = { X = camLoc.X + (rx * sp.side) * dist, Y = camLoc.Y + (ry * sp.side) * dist, Z = camLoc.Z + sp.up * dist }
        pcall(function() l:K2_SetActorLocation(ll, false, hit, true) end)
        pcall(function() local lc = l.PointLightComponent; lc:SetAttenuationRadius(dist * 4); lc:SetIntensity(3000 * dm * dm * sp.k) end)
    end
    local cc = st.cam.CaptureComponent2D
    st.mesh:SetActorHiddenInGame(false)
    local ok = pcall(function()
        pcall(function() cc.CaptureSource = 2 end)
        cc:CaptureScene()
        krl:ExportRenderTarget(world, st.rt, dir, id .. ".png")
    end)
    st.mesh:SetActorHiddenInGame(true)
    pcall(function() sc:SetMaterial(0, nil) end)
    if not ok then fail() end
end

-- "Export texture" for a texture the game made from a picture file (an imported model's own textures): copy that
-- file into the export folder. arg: texture path \t destination without extension
handlers.ed_texcopy = function(arg)
    local tp, dest = (arg or ""):match("^([^\t]+)\t(.+)$")
    if not tp then return end
    local src = E.TextureSource and E.TextureSource(tp)
    if not src then Msg("Can't export that texture: the game made it and there's no file behind it.", true); return end
    local ext = src:match("(%.[%w]+)$") or ".png"
    local fi = io.open(src, "rb"); if not fi then Msg("Couldn't read " .. src, true); return end
    local data = fi:read("*a"); fi:close()
    local fo = io.open(dest .. ext, "wb"); if not fo then Msg("Couldn't write " .. dest .. ext, true); return end
    fo:write(data); fo:close()
    Msg("Exported texture: " .. (dest .. ext):match("([^\\/]+)$"))
end
-- every texture / material loaded in the game right now (for the pickers)
handlers.ed_texlist = function()
    local lines, n = {}, 0
    for _, t in ipairs(FindAllOf("Texture2D") or {}) do
        if n >= 30000 then break end
        local p = ObjectPath(t)
        if p ~= "" and p:sub(1, 1) == "/" and not p:find(":", 1, true) then
            local w, h = 0, 0
            pcall(function() w = t:Blueprint_GetSizeX(); h = t:Blueprint_GetSizeY() end)
            n = n + 1; lines[n] = string.format("%s\t%d\t%d", p, tonumber(w) or 0, tonumber(h) or 0)
        end
    end
    table.sort(lines)
    WriteFile(Dir .. "textures.txt", table.concat(lines, "\n") .. "\n")
end
handlers.ed_matlist = function()
    local lines, n = {}, 0
    for _, cls in ipairs({ "MaterialInstanceConstant", "Material" }) do
        for _, m in ipairs(FindAllOf(cls) or {}) do
            if n >= 40000 then break end
            local p = ObjectPath(m)
            if p ~= "" and p:sub(1, 1) == "/" and not p:find(":", 1, true) then n = n + 1; lines[n] = p .. "\t" .. (cls == "Material" and "m" or "i") end
        end
    end
    table.sort(lines)
    WriteFile(Dir .. "materiallist.txt", table.concat(lines, "\n") .. "\n")
end
end)()

-- Game objects remembered for reuse belong to the level they were found in: when another level loads, Unreal frees
-- the ones nothing else uses (a material only that level used, textures made from files, parts of objects), and using
-- a freed one crashes the game. So they're all forgotten on every level change (and found again when needed).
function E.ForgetLevelObjects()
    origCache, texPick, micList, slotCache, texCache = {}, {}, nil, {}, {}
    Models.live, Models.order = {}, {}
    if E.MatsMapChanged then pcall(E.MatsMapChanged) end
end
function E.OnMapChanged()
    pcall(E.ForgetLevelObjects)
    Undo.stack, Undo.lastPath, E.softDeleted = {}, nil, {}
    selected, selectedPath, actorCache, camPC = nil, "", {}, nil
    look = nil
    madeMovable, warnedStuck, inputLocked, studio, thumbQ, thumbPending = {}, {}, nil, nil, {}, false
    if E.open then ExecuteInGameThread(function() pcall(LockGameInput, true) end) end
end

function E.Init(bridgeDir, freecam) Dir, Freecam = bridgeDir, freecam end

return E
