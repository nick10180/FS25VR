--
-- FS25_VR - stereoscopic 6DOF VR for Farming Simulator 25
--
-- The native bridge (dinput8.dll) drives an OpenXR session and composites the game's frames
-- into the headset. This script places the game camera at the exact eye position and
-- orientation the bridge asks for, with that eye's asymmetric projection, every frame:
--
--   * In a cab the head is anchored to the seat (a node fixed to the vehicle), so leaning,
--     ducking and looking around move you inside the cab while the cab moves with the vehicle.
--   * On foot the head is anchored to the player's body: mouse/stick turns the body, the
--     headset adds full 6DOF on top (look, lean, crouch). Pitch comes only from the headset.
--   * Exterior / third person cameras keep their position and heading; the horizon stays level.
--
-- Menus, the map, shops and loading screens are shown on a flat screen in front of you.
--
-- The game's own camera transform is saved before VR offsets are applied and restored before
-- the game updates the camera again, so camera smoothing and game logic never feed on VR offsets.
--

VRMod = {}
VRMod.modDirectory = g_currentModDirectory
VRMod.modName = g_currentModName

VRMod.KEY_RECENTER = Input.KEY_f8 or 289
VRMod.KEY_TOGGLE = Input.KEY_f9 or 290
VRMod.KEY_SSAO = Input.KEY_f7 or 288
VRMod.KEY_FRUSTUM = Input.KEY_f6 or 287
VRMod.KEY_HUD = Input.KEY_f10 or 291
VRMod.KEY_FOVEATED = Input.KEY_f5 or 286
VRMod.KEY_SYNC_PHASE = Input.KEY_f3 or 284

-- The HUD is drawn flat into each eye's image, so in the headset it sits in the corners of your
-- vision at screen depth. Hidden while the 3D view is in VR (F10 shows it again).
VRMod.hideHud = true
VRMod.symmetric = true

-- Ambient occlusion: quality 1 ("Low") is SAO, a purely spatial pass; quality 2 and up is XeGTAO.
-- With the bridge's centred per-eye projection both are consistent between the eyes (the AO
-- shaders assume a centred frustum), so the player's setting is kept. F7 can force SAO.
VRMod.SSAO_SAO = 1
VRMod.forceSAO = false

-- Temporal anti-aliasing (TAA, DLAA, FSR3, XeSS) blends in the previous frame, which with
-- alternate-eye rendering is the other eye: every object gets a faint copy at the other eye's
-- position (wide for near objects) and the stereo depth flattens. Switched off while VR runs and
-- restored afterwards.
VRMod.TEMPORAL_AA = {}
if PostProcessAntiAliasing ~= nil then
    for _, name in ipairs({"TAA", "TAA_MOBILE", "DLAA", "FSR3", "XESS"}) do
        if PostProcessAntiAliasing[name] ~= nil then
            VRMod.TEMPORAL_AA[PostProcessAntiAliasing[name]] = true
        end
    end
end

-- Head position offset, moved with the numpad while held: one per vehicle (in the cab) and one on
-- foot, saved in modSettings/FS25_VR.xml. Axis 1 = right, 2 = up, 3 = back (camera space).
VRMod.OFFSET_KEYS = {
    [Input.KEY_KP_4 or 260] = {1, -1},  -- left
    [Input.KEY_KP_6 or 262] = {1, 1},   -- right
    [Input.KEY_KP_9 or 265] = {2, 1},   -- up
    [Input.KEY_KP_3 or 259] = {2, -1},  -- down
    [Input.KEY_KP_8 or 264] = {3, -1},  -- forward
    [Input.KEY_KP_2 or 258] = {3, 1},   -- back
}
VRMod.KEY_OFFSET_RESET = Input.KEY_KP_5 or 261
VRMod.OFFSET_SPEED = 0.15  -- metres per second
VRMod.OFFSET_LIMIT = 1.0   -- metres per axis

local function log(fmt, ...)
    print(string.format("[FS25_VR] " .. fmt, ...))
end

function VRMod:loadMap(name)
    self.enabled = true
    self.cams = {}
    self.messageText = nil
    self.messageTime = 0
    self.lastFrame = -1
    self.wasRunning = false
    self.offsetHeld = {}
    self.offsetDirty = false
    self.activeOffsetKey = nil

    local ok, api = pcall(setStereoRendering, true)
    if ok and type(api) == "table" and api.getView ~= nil then
        self.api = api
        log("native bridge v%d connected (%s)", api.version or 0, api.status and api.status() or "?")
    else
        self.api = nil
        log("native bridge not found - install dinput8.dll and openxr_loader.dll into the game's x64 folder")
        return
    end

    self:installHooks()
    self:installHudDepth()
    self:loadOffsets()
    self:applyHudSize()
    self.hudEyeMode = self.api.getHudEye ~= nil and self.api.getHudEye() or 0
    self:applySavedSettings()
    local okMenu, errMenu = pcall(self.addSettingsSection, self)
    if not okMenu then
        log("settings menu: could not add the VR section: %s", tostring(errMenu))
    end

    self.eyeNode = createTransformGroup("vrEye")
    link(getRootNode(), self.eyeNode)
    self.worldAnchor = createTransformGroup("vrWorldAnchor")
    link(getRootNode(), self.worldAnchor)

    -- latency calibration marker (see VRMod:draw)
    self.markerOverlay = createImageOverlay(self.modDirectory .. "marker.dds")
end

function VRMod:updateSSAO()
    if not VRMod.forceSAO then
        self:restoreSSAO()
        return
    end
    local quality = getSSAOQuality()
    if quality ~= nil and quality > VRMod.SSAO_SAO then
        self.savedSSAOQuality = quality  -- the player's setting (re-read in case it was changed)
        setSSAOQuality(VRMod.SSAO_SAO)
    end
end

function VRMod:restoreSSAO()
    if self.savedSSAOQuality ~= nil then
        setSSAOQuality(self.savedSSAOQuality)
        self.savedSSAOQuality = nil
    end
end

function VRMod:updateTemporalAA()
    if getPostProcessAntiAliasing == nil or setPostProcessAntiAliasing == nil or PostProcessAntiAliasing == nil then
        return
    end
    local mode = getPostProcessAntiAliasing()
    if mode ~= nil and VRMod.TEMPORAL_AA[mode] then
        if self.savedPostProcessAA == nil then
            log("temporal anti-aliasing (mode %d) off while VR runs", mode)
        end
        self.savedPostProcessAA = mode  -- the player's setting (re-read in case it was changed)
        setPostProcessAntiAliasing(PostProcessAntiAliasing.OFF)
    end
end

function VRMod:restoreTemporalAA()
    if self.savedPostProcessAA ~= nil then
        setPostProcessAntiAliasing(self.savedPostProcessAA)
        self.savedPostProcessAA = nil
    end
end

function VRMod:updateHud(inVr)
    local hud = g_currentMission ~= nil and g_currentMission.hud or nil
    if hud == nil or hud.setIsVisible == nil then
        return
    end
    if inVr and VRMod.hideHud then
        local visible = hud.getIsVisible == nil or hud:getIsVisible()
        if not self.hudHidden and visible then
            hud:setIsVisible(false)
            self.hudHidden = true
        end
    elseif self.hudHidden then
        hud:setIsVisible(true)
        self.hudHidden = false
    end
end

-- The HUD is laid out from these globals (screen units 0..1, y up). In VR the screen edges lie
-- outside what the lenses show, so while VR runs they are moved into the area both eyes see
-- (hudArea in fs25vr.ini), and the game is told the UI scale changed, which makes every HUD part
-- lay itself out again. The game's values are put back when VR stops.
-- Mods run in an environment of their own whose missing keys are looked up in the game's
-- globals: writing a global, even through _G, only changes the mod's copy, which the game never
-- reads. The game's table sits behind the environment's metatable.
local function findGameGlobals()
    -- best: the environment the game's own HUD drawing code runs in
    if getfenv ~= nil and Overlay ~= nil and type(Overlay.render) == "function" then
        local okEnv, g = pcall(getfenv, Overlay.render)
        if okEnv and type(g) == "table" and g ~= _G then
            return g, 1
        end
    end
    local env, depth = _G, 0
    for _ = 1, 4 do
        local okMt, mt = pcall(getmetatable, env)
        local idx = okMt and type(mt) == "table" and rawget(mt, "__index") or nil
        if type(idx) ~= "table" or idx == env then
            break
        end
        env, depth = idx, depth + 1
    end
    if depth == 0 and getfenv ~= nil then
        local okEnv, g = pcall(getfenv, 0)
        if okEnv and type(g) == "table" and g ~= _G then
            env, depth = g, 1
        end
    end
    return env, depth
end
local G, gameGlobalsDepth = findGameGlobals()

-- HUD size presets in VR (Shift+F10 cycles, saved in modSettings/FS25_VR.xml): the share of the field
-- of view the HUD is moved into, and its size relative to the game's UI scale.
VRMod.HUD_SIZES = {
    {area = 0.65, scale = 1.0},
    {area = 0.55, scale = 0.9},
    {area = 0.45, scale = 0.8},
    {area = 0.35, scale = 0.7},
}
VRMod.hudSize = 3
VRMod.hudScale = VRMod.HUD_SIZES[VRMod.hudSize].scale

local function ctrlDown()
    local ok, down = pcall(function()
        return (Input.KEY_lctrl ~= nil and Input.isKeyPressed(Input.KEY_lctrl)) or
            (Input.KEY_rctrl ~= nil and Input.isKeyPressed(Input.KEY_rctrl))
    end)
    return ok and down == true
end

local function altDown()
    local ok, down = pcall(function()
        return (Input.KEY_lalt ~= nil and Input.isKeyPressed(Input.KEY_lalt)) or
            (Input.KEY_ralt ~= nil and Input.isKeyPressed(Input.KEY_ralt))
    end)
    return ok and down == true
end

-- HUD depth in VR (Alt+F10 cycles, saved): metres the HUD appears at, 0 = infinity. Two copies of
-- the HUD mean the eyes are converged at another depth: while driving they look far ahead, so
-- infinity (the default, as the game draws it) usually fuses best. nil = the value from fs25vr.ini.
VRMod.HUD_DEPTHS = {0.75, 1.0, 1.5, 2.5, 5.0, 10.0, 20.0, 0}
VRMod.hudDepthIndex = nil
-- Older versions saved the list position of this shorter list.
VRMod.HUD_DEPTHS_V1 = {0.75, 1.0, 1.5, 2.5, 5.0, 0}

function VRMod.findHudDepth(metres)
    for i, d in ipairs(VRMod.HUD_DEPTHS) do
        if math.abs(d - metres) < 0.01 then
            return i
        end
    end
    return nil
end

-- HUD height in VR (Ctrl+F10 cycles, saved): how far the HUD area is moved down, as a share of the
-- image height. nil = the value from fs25vr.ini (hudLower).
VRMod.HUD_LOWERS = {0, 0.04, 0.08, 0.12, 0.16, 0.20}
VRMod.hudLowerIndex = nil

local function shiftDown()
    local ok, down = pcall(function()
        return (Input.KEY_lshift ~= nil and Input.isKeyPressed(Input.KEY_lshift)) or
            (Input.KEY_rshift ~= nil and Input.isKeyPressed(Input.KEY_rshift))
    end)
    return ok and down == true
end

function VRMod:applyHudSize()
    local preset = VRMod.HUD_SIZES[VRMod.hudSize] or VRMod.HUD_SIZES[3]
    VRMod.hudScale = preset.scale
    if self.api ~= nil and self.api.setHudArea ~= nil then
        self.api.setHudArea(preset.area)
    end
    local depth = VRMod.hudDepthIndex ~= nil and VRMod.HUD_DEPTHS[VRMod.hudDepthIndex] or nil
    if depth ~= nil and self.api ~= nil and self.api.setHudDepth ~= nil then
        self.api.setHudDepth(depth)
    end
    local lower = VRMod.hudLowerIndex ~= nil and VRMod.HUD_LOWERS[VRMod.hudLowerIndex] or nil
    if lower ~= nil and self.api ~= nil and self.api.setHudLower ~= nil then
        self.api.setHudLower(lower)
    end
    self.hudLayoutTimer = 0  -- lay it out again right away
end

VRMod.HUD_GLOBALS = {"g_hudAnchorLeft", "g_hudAnchorRight", "g_hudAnchorBottom", "g_hudAnchorTop",
    "g_safeFrameOffsetX", "g_safeFrameOffsetY"}

local function uiScale()
    local ok, s = pcall(function()
        return g_gameSettings:getValue(GameSettings.SETTING.UI_SCALE)
    end)
    return ok and tonumber(s) or 1
end

local function setHudGlobals(values)
    local ok, err = pcall(function()
        for i, name in ipairs(VRMod.HUD_GLOBALS) do
            G[name] = values[i]
        end
    end)
    if not ok and not VRMod.hudGlobalsFailed then
        VRMod.hudGlobalsFailed = true
        log("cannot move the HUD (%s)", tostring(err))
    end
    return ok
end

local function hudGlobalsAre(values)
    for i, name in ipairs(VRMod.HUD_GLOBALS) do
        if type(G[name]) ~= "number" or math.abs(G[name] - values[i]) > 0.002 then
            return false
        end
    end
    return true
end

-- hudScale: the HUD's size in VR relative to the game's UI scale (1 when putting it back)
function VRMod:relayoutHud(values, hudScale)
    local scale = uiScale() * (hudScale or 1)
    local ok, err = pcall(function()
        local key = GameSettings.SETTING.UI_SCALE
        g_messageCenter:publish(MessageType.SETTING_CHANGED[key], scale)
    end)
    if not ok then
        log("could not lay out the HUD: %s", tostring(err))
    end
    if not hudGlobalsAre(values) then
        setHudGlobals(values)  -- the game recomputed the anchors while laying out
    end
    -- The message does not reach every HUD part (the parts keep the positions they computed),
    -- so lay out each one directly: storeScaledValues places a part from the anchors. The HUD
    -- keeps its parts as fields and in lists, so look one level into tables too.
    local hud = g_currentMission ~= nil and g_currentMission.hud or nil
    if hud == nil then
        return
    end
    local done, report = {}, {}
    local function layout(name, part)
        if type(part) ~= "table" or done[part] or type(part.storeScaledValues) ~= "function" then
            return false
        end
        done[part] = true
        local x0, y0
        if type(part.getPosition) == "function" then
            local okPos, x, y = pcall(part.getPosition, part)
            if okPos then x0, y0 = x, y end
        end
        if type(part.setScale) == "function" then
            pcall(part.setScale, part, scale)
        end
        local okLayout = pcall(part.storeScaledValues, part, scale)
        if type(part.getBackgroundPosition) == "function" and type(part.setPosition) == "function" then
            -- the minimap keeps the position it was created at: move it to its (new) background spot
            local okPos, bx, by = pcall(part.getBackgroundPosition, part)
            if okPos and type(bx) == "number" and type(by) == "number" then
                pcall(part.setPosition, part, bx, by)
                pcall(part.storeScaledValues, part, scale)
            end
        end
        if type(part.storeOriginalPosition) == "function" then
            pcall(part.storeOriginalPosition, part)  -- hide/show animations return to this position
        end
        if not self.loggedHudParts then
            local x1, y1
            if type(part.getPosition) == "function" then
                local okPos, x, y = pcall(part.getPosition, part)
                if okPos then x1, y1 = x, y end
            end
            table.insert(report, string.format("%s %s->%s%s", name,
                x0 and string.format("(%.2f,%.2f)", x0, y0) or "?", x1 and string.format("(%.2f,%.2f)", x1, y1) or "?",
                okLayout and "" or " FAILED"))
        end
        return true
    end
    for name, part in pairs(hud) do
        if not layout(tostring(name), part) and type(part) == "table" and getmetatable(part) == nil then
            for i, sub in pairs(part) do
                layout(tostring(name) .. "." .. tostring(i), sub)
            end
        end
    end
    if not self.loggedHudParts then
        self.loggedHudParts = true
        log("HUD parts laid out (%d): %s", #report, table.concat(report, ", "))
    end
end

function VRMod:updateHudLayout(dt)
    if self.api.getHudRect == nil or type(G.g_hudAnchorLeft) ~= "number" or VRMod.hudGlobalsFailed then
        return
    end
    self.hudLayoutTimer = (self.hudLayoutTimer or 0) - dt
    if self.hudLayoutTimer > 0 then
        return
    end
    self.hudLayoutTimer = 1000  -- re-checked every second: the game may lay it out again (resize)
    local ok, left, right, bottom, top = self.api.getHudRect()
    if not ok then
        return
    end
    local want = {left, right, bottom, top, left, bottom}
    if hudGlobalsAre(want) and self.appliedHudScale == VRMod.hudScale then
        return
    end
    if self.savedHudGlobals == nil then
        self.savedHudGlobals = {}
        for i, name in ipairs(VRMod.HUD_GLOBALS) do
            self.savedHudGlobals[i] = G[name]
        end
    end
    if not setHudGlobals(want) then
        return
    end
    self:relayoutHud(want, VRMod.hudScale)
    self.appliedHudScale = VRMod.hudScale
    self.hudMoves = (self.hudMoves or 0) + 1
    if self.hudMoves <= 5 then
        log("HUD moved into view: left %.2f right %.2f bottom %.2f top %.2f, size %.2f", left, right, bottom, top,
            VRMod.hudScale)
    end
end

-- HUD depth. The game draws the HUD at the same screen position in both images, which puts it at
-- infinity: everything nearer (the cab, the dashboard) is "behind" it in depth but in front of it
-- in the image, which strains the eyes. All 2D drawing goes through renderOverlay/renderText, so
-- they are wrapped to shift each eye's image sideways by the bridge's offset for hudDepth.
-- Menus are drawn without a shift (they are shown on the flat menu screen).
VRMod.hudShift = 0
VRMod.hudSkip = false  -- this frame is for the eye without HUD (hudEye in fs25vr.ini)
VRMod.hudDraws = 0
VRMod.draws2D = {overlays = 0, texts = 0}  -- all 2D drawing, for the HUD diagnostics

function VRMod:installHudDepth()
    if VRMod.hudDepthInstalled or type(G.renderOverlay) ~= "function" or type(G.renderText) ~= "function" then
        return
    end
    log("game globals %s", gameGlobalsDepth > 0 and "found behind the mod environment" or "are the mod environment")
    local origOverlay, origText = G.renderOverlay, G.renderText
    local ok, err = pcall(function()
        G.renderOverlay = function(id, x, ...)
            VRMod.draws2D.overlays = VRMod.draws2D.overlays + 1
            if VRMod.hudSkip then
                return  -- HUD in the other eye only (hudEye)
            end
            local dx = VRMod.hudShift
            if dx ~= 0 and type(x) == "number" then
                x = x + dx
                VRMod.hudDraws = VRMod.hudDraws + 1
            end
            return origOverlay(id, x, ...)
        end
        G.renderText = function(x, ...)
            VRMod.draws2D.texts = VRMod.draws2D.texts + 1
            if VRMod.hudSkip then
                return
            end
            local dx = VRMod.hudShift
            if dx ~= 0 and type(x) == "number" then
                x = x + dx
                VRMod.hudDraws = VRMod.hudDraws + 1
            end
            return origText(x, ...)
        end
    end)
    VRMod.renderOverlayRaw = origOverlay
    VRMod.hudDepthInstalled = ok
    if not ok then
        log("cannot give the HUD depth (%s)", tostring(err))
    end
end

-- Logs (every 10 s while the HUD is shown in VR) whether the game draws its HUD at all, to find
-- where it gets lost: HUD visibility as the game reports it and the 2D drawing per frame.
function VRMod:logHudDiagnostics(dt)
    self.hudDiagFrames = (self.hudDiagFrames or 0) + 1
    self.hudDiagTimer = (self.hudDiagTimer or 10000) - dt
    if self.hudDiagTimer > 0 then
        return
    end
    local frames = math.max(1, self.hudDiagFrames)
    local hud = g_currentMission ~= nil and g_currentMission.hud or nil
    local visible = "?"
    if hud ~= nil and hud.getIsVisible ~= nil then
        local ok, v = pcall(hud.getIsVisible, hud)
        visible = ok and tostring(v) or "error"
    end
    log("HUD check: game says visible=%s, stereo=%s, per frame %.1f overlays and %.1f texts drawn, shift %.4f, anchors L%.2f R%.2f B%.2f T%.2f",
        visible, tostring(self:isStereoAllowed()), VRMod.draws2D.overlays / frames, VRMod.draws2D.texts / frames,
        VRMod.hudShift, tonumber(G.g_hudAnchorLeft) or -1, tonumber(G.g_hudAnchorRight) or -1,
        tonumber(G.g_hudAnchorBottom) or -1, tonumber(G.g_hudAnchorTop) or -1)
    VRMod.draws2D.overlays, VRMod.draws2D.texts = 0, 0
    self.hudDiagFrames = 0
    self.hudDiagTimer = 10000
end

function VRMod:updateHudShift()
    local ok, left, right = false, 0, 0
    if self.api.getHudShift ~= nil then
        ok, left, right = self.api.getHudShift()
    end
    if ok then
        self.hudShiftEye = {[0] = left, [1] = right}
    else
        self.hudShiftEye = nil
    end
    if VRMod.hudDraws > 0 and not self.loggedHudDepth then
        self.loggedHudDepth = true
        log("HUD depth active (%d shifted draws)", VRMod.hudDraws)
    end
end

function VRMod:restoreHudLayout()
    if self.savedHudGlobals == nil then
        return
    end
    local values = self.savedHudGlobals
    self.savedHudGlobals = nil
    setHudGlobals(values)
    self:relayoutHud(values)
    self.appliedHudScale = nil
end

function VRMod:restoreFrameLimiter()
    if not self.limiterOverridden then
        return
    end
    self.limiterOverridden = false
    local ok, limit = pcall(function()
        return g_gameSettings:getValue(GameSettings.SETTING.FRAME_LIMIT or "frameLimit")
    end)
    limit = ok and tonumber(limit) or 60
    setFramerateLimiter(limit > 0, limit)
end

function VRMod:deleteMap()
    if self.api == nil then
        return
    end
    self:restoreFrameLimiter()
    self:restoreSSAO()
    self:restoreTemporalAA()
    self:restoreHudLayout()
    VRMod.hudShift = 0
    VRMod.hudSkip = false
    self:updateHud(false)
    if self.offsetDirty then
        self:saveOffsets()
    end
    for cam, _ in pairs(self.cams) do
        self:restoreCamera(cam, true)
    end
    self.cams = {}
    if self.eyeNode ~= nil and entityExists(self.eyeNode) then
        delete(self.eyeNode)
    end
    if self.worldAnchor ~= nil and entityExists(self.worldAnchor) then
        delete(self.worldAnchor)
    end
    self.eyeNode = nil
    self.worldAnchor = nil
    if self.markerOverlay ~= nil and self.markerOverlay ~= 0 then
        delete(self.markerOverlay)
        self.markerOverlay = nil
    end
end

function VRMod:installHooks()
    if VRMod.hooksInstalled then
        return
    end
    VRMod.hooksInstalled = true

    VehicleCamera.update = Utils.prependedFunction(VehicleCamera.update, function(cam, dt)
        VRMod:restoreCamera(cam.cameraNode, false)
    end)
    VehicleCamera.update = Utils.appendedFunction(VehicleCamera.update, function(cam, dt)
        VRMod:onVehicleCameraUpdated(cam)
    end)
    VehicleCamera.onDeactivate = Utils.prependedFunction(VehicleCamera.onDeactivate, function(cam)
        VRMod:restoreCamera(cam.cameraNode, true)
    end)

    -- PlayerCamera:update is not part of the published sources; fall back to updatePosition,
    -- which runs last in the camera update.
    local playerUpdate = PlayerCamera.update ~= nil and "update" or "updatePosition"
    PlayerCamera[playerUpdate] = Utils.prependedFunction(PlayerCamera[playerUpdate], function(pc, dt)
        local cam = getCamera()
        if VRMod.cams[cam] ~= nil then
            VRMod:restoreCamera(cam, false)
        end
    end)
    PlayerCamera[playerUpdate] = Utils.appendedFunction(PlayerCamera[playerUpdate], function(pc, dt)
        VRMod:onPlayerCameraUpdated(pc)
    end)
    -- Hand tool crosshairs are drawn at the screen centre. Each eye's frustum is off-centre, so
    -- move them to where that eye's straight-ahead direction lands; both eyes then agree.
    if HandTool ~= nil and HandTool.createCrosshairOverlay ~= nil then
        HandTool.createCrosshairOverlay = Utils.overwrittenFunction(HandTool.createCrosshairOverlay,
            function(tool, superFunc, ...)
                return VRMod:wrapCrosshair(superFunc(tool, ...))
            end)
    end
    log("camera hooks installed (PlayerCamera.%s)", playerUpdate)
end

function VRMod:wrapCrosshair(overlay)
    if type(overlay) ~= "table" or overlay.vrWrapped or overlay.render == nil then
        return overlay
    end
    overlay.vrWrapped = true
    local render = overlay.render
    overlay.render = function(o, ...)
        local dx, dy = VRMod.crosshairDX or 0, VRMod.crosshairDY or 0
        if (dx ~= 0 or dy ~= 0) and o.getPosition ~= nil and o.setPosition ~= nil then
            local x, y = o:getPosition()
            o:setPosition(x + dx, y + dy)
            render(o, ...)
            o:setPosition(x, y)
        else
            render(o, ...)
        end
    end
    return overlay
end

-- True when the 3D view should be rendered stereoscopically this frame.
function VRMod:isStereoAllowed()
    if not self.enabled or self.api == nil or self.eyeNode == nil then
        return false
    end
    if g_gui ~= nil and g_gui:getIsGuiVisible() then
        return false
    end
    return true
end

---------------------------------------------------------------------------------------------------
-- camera state

local function nearlyEqual(a, b)
    return math.abs(a - b) < 0.00001
end

function VRMod:getCamState(cam)
    local s = self.cams[cam]
    if s == nil then
        s = {applied = false}
        s.fov = getFovY(cam)
        s.offX, s.offY = getProjectionOffset(cam)
        s.tx, s.ty, s.tz = getTranslation(cam)
        s.rx, s.ry, s.rz = getRotation(cam)
        self.cams[cam] = s
    end
    return s
end

-- Puts back the game's own transform and projection, unless the game has since overwritten them.
function VRMod:restoreCamera(cam, includeProjection)
    local s = self.cams[cam]
    if s == nil or not entityExists(cam) then
        return
    end
    if s.applied then
        local x, y, z = getTranslation(cam)
        local rx, ry, rz = getRotation(cam)
        if nearlyEqual(x, s.vx) and nearlyEqual(y, s.vy) and nearlyEqual(z, s.vz)
            and nearlyEqual(rx, s.vrx) and nearlyEqual(ry, s.vry) and nearlyEqual(rz, s.vrz) then
            setTranslation(cam, s.tx, s.ty, s.tz)
            setRotation(cam, s.rx, s.ry, s.rz)
        end
        s.applied = false
    end
    if includeProjection and s.projApplied then
        if nearlyEqual(getFovY(cam), s.vfov) then
            setFovY(cam, s.fov)
        end
        setProjectionOffset(cam, s.offX, s.offY)
        s.projApplied = false
    end
end

-- Places 'cam' at the current VR eye, relative to 'anchor' (a node oriented like a camera:
-- -Z forward, +Y up, origin at the neutral head position). Returns true if applied.
function VRMod:applyEye(cam, anchorFn)
    if cam == nil or cam == 0 then
        return false
    end
    local ok, eye, px, py, pz, qx, qy, qz, qw, fovY, offX, offY, frame, second = self.api.getView()
    if not ok then
        return false
    end
    local anchor, offsetKey = anchorFn(eye, frame, second)
    if anchor == nil then
        return false
    end
    -- the offset only changes on the first frame of a pair, so both images of a pair agree
    self.activeOffsetKey = offsetKey
    if not second or offsetKey ~= self.pairOffsetKey then
        self.pairOffsetKey = offsetKey
        local o = offsetKey ~= nil and self.offsets[offsetKey] or nil
        if o ~= nil then
            self.pairOx, self.pairOy, self.pairOz = o[1], o[2], o[3]
        else
            self.pairOx, self.pairOy, self.pairOz = 0, 0, 0
        end
    end
    px, py, pz = px + self.pairOx, py + self.pairOy, pz + self.pairOz

    local s = self:getCamState(cam)
    if not s.applied then
        -- remember the game's values for this frame
        s.tx, s.ty, s.tz = getTranslation(cam)
        s.rx, s.ry, s.rz = getRotation(cam)
    end
    local fov = getFovY(cam)
    if not s.projApplied or not nearlyEqual(fov, s.vfov) then
        s.fov = fov  -- the game's own field of view
    end

    if getParent(self.eyeNode) ~= anchor then
        link(anchor, self.eyeNode)
    end
    setTranslation(self.eyeNode, px, py, pz)
    setQuaternion(self.eyeNode, qx, qy, qz, qw)
    local wx, wy, wz = getWorldTranslation(self.eyeNode)
    local wqx, wqy, wqz, wqw = getWorldQuaternion(self.eyeNode)
    setWorldTranslation(cam, wx, wy, wz)
    setWorldQuaternion(cam, wqx, wqy, wqz, wqw)
    setFovY(cam, fovY)
    setProjectionOffset(cam, offX, offY)

    s.vx, s.vy, s.vz = getTranslation(cam)
    s.vrx, s.vry, s.vrz = getRotation(cam)
    s.vfov = fovY
    s.applied = true
    s.projApplied = true
    self.lastFrame = frame
    self.markerFrame = frame
    -- this eye's forward direction lands at (0.5 - offX, 0.5 - offY) on screen
    VRMod.crosshairDX, VRMod.crosshairDY = -offX, -offY
    -- this frame's HUD is drawn for this eye
    VRMod.hudShift = self.hudShiftEye ~= nil and self.hudShiftEye[eye] or 0
    VRMod.hudSkip = (self.hudEyeMode == 1 and eye ~= 0) or (self.hudEyeMode == 2 and eye ~= 1)
    -- the camera's clip distances let the headset runtime read the depth layer
    if not second and self.api.setDepthRange ~= nil and getNearClip ~= nil and getFarClip ~= nil then
        self.api.setDepthRange(getNearClip(cam), getFarClip(cam))
    end
    if not self.didInitialRecenter then
        -- the bridge's start-up recentre may have happened with the headset off; do it again now
        self.didInitialRecenter = true
        self.api.recenter()
    end
    return true
end

-- World anchors (on foot, exterior cameras) only move on the first frame of a stereo pair, so
-- both images of a pair see the world from the same body position and heading. Otherwise turning
-- with the mouse or stick rotates the world between the left and right image, which the
-- compositor cannot correct (it only knows about head motion) and shows as tearing/doubling.
function VRMod:holdWorldAnchor(cam, eye, frame, second)
    if second == nil then
        second = eye == 1  -- bridge before 0.1.9
    end
    local hold = second and self.anchorCam == cam and self.anchorFrame == frame - 1
    if not hold then
        self.anchorCam = cam
        self.anchorFrame = frame
    end
    return hold
end

-- Level, heading-only anchor at a world position.
function VRMod:setWorldAnchor(x, y, z, dirX, dirZ)
    if math.abs(dirX) + math.abs(dirZ) > 0.0001 then
        self.worldAnchorYaw = math.atan2(-dirX, -dirZ)
    end
    setWorldTranslation(self.worldAnchor, x, y, z)
    setWorldRotation(self.worldAnchor, 0, self.worldAnchorYaw or 0, 0)
    return self.worldAnchor
end

-- Anchor at the game camera's position with its heading only.
function VRMod:getLevelAnchorForCamera(cam)
    local x, y, z = getWorldTranslation(cam)
    local dx, _, dz = localDirectionToWorld(cam, 0, 0, -1)
    if math.abs(dx) + math.abs(dz) < 0.05 then
        -- looking straight up or down: use the camera's up vector for the heading
        local ux, _, uz = localDirectionToWorld(cam, 0, 1, 0)
        local _, fy, _ = localDirectionToWorld(cam, 0, 0, -1)
        if fy < 0 then
            dx, dz = ux, uz
        else
            dx, dz = -ux, -uz
        end
    end
    return self:setWorldAnchor(x, y, z, dx, dz)
end

---------------------------------------------------------------------------------------------------
-- vehicles

-- Seat-fixed head node for an interior camera. The engine's own head-tracking node is used when
-- the camera was created with head tracking active; otherwise an equivalent one is built.
function VRMod:getSeatAnchor(vcam)
    if vcam.headTrackingNode ~= nil then
        return vcam.headTrackingNode
    end
    if vcam.vrSeatNode == nil then
        local posNode = vcam.cameraPositionNode
        local rotNode = vcam.rotateNode or posNode
        local parent = getParent(rotNode)
        if parent == 0 then
            return nil
        end
        local node = createTransformGroup("vrSeatNode")
        link(parent, node)
        -- Measure the camera's default view as the vehicle defines it, not wherever it is turned
        -- right now (mouse/stick look, or a view the game kept from last time, e.g. facing the
        -- implement behind). Otherwise "forward" is off for the whole session and no recentre
        -- can fix it.
        local rx, ry, rz = getRotation(rotNode)
        if vcam.origRotX ~= nil then
            setRotation(rotNode, vcam.origRotX, vcam.origRotY, vcam.origRotZ)
        end
        local tx, ty, tz = localToLocal(posNode, parent, 0, 0, 0)
        local dx, _, dz = localDirectionToLocal(posNode, parent, 0, 0, 1)
        setRotation(rotNode, rx, ry, rz)
        setTranslation(node, tx, ty, tz)
        if math.abs(dx) + math.abs(dz) > 0.0001 then
            setDirection(node, dx, 0, dz, 0, 1, 0)
        else
            setRotation(node, 0, 0, 0)
        end
        vcam.vrSeatNode = node
    end
    return vcam.vrSeatNode
end

function VRMod:onVehicleCameraUpdated(vcam)
    if not self:isStereoAllowed() then
        return
    end
    local cam = vcam.cameraNode
    if cam ~= getCamera() then
        return
    end
    self:applyEye(cam, function(eye, frame, second)
        if vcam.isInside then
            local file = vcam.vehicle ~= nil and vcam.vehicle.configFileName or "unknown"
            return self:getSeatAnchor(vcam), "cab:" .. file
        end
        if self:holdWorldAnchor(cam, eye, frame, second) then
            return self.worldAnchor
        end
        return self:getLevelAnchorForCamera(cam)
    end)
end

---------------------------------------------------------------------------------------------------
-- on foot

function VRMod:onPlayerCameraUpdated(pc)
    if not self:isStereoAllowed() then
        return
    end
    local cam = getCamera()
    if cam ~= pc.firstPersonCamera and cam ~= pc.thirdPersonCamera and cam ~= pc.thirdPersonConversationCamera then
        return
    end
    self:applyEye(cam, function(eye, frame, second)
        local offsetKey = cam == pc.firstPersonCamera and "foot" or nil
        if self:holdWorldAnchor(cam, eye, frame, second) then
            return self.worldAnchor, offsetKey
        end
        if cam == pc.firstPersonCamera and pc.pitchNode ~= nil and pc.yawNode ~= nil then
            -- body anchor: head position without view bobbing, heading from the body yaw
            local x, y, z = getWorldTranslation(pc.pitchNode)
            local dx, _, dz = localDirectionToWorld(pc.yawNode, 0, 0, 1)
            return self:setWorldAnchor(x, y, z, dx, dz), offsetKey
        end
        return self:getLevelAnchorForCamera(cam), offsetKey
    end)
end

---------------------------------------------------------------------------------------------------
-- head position offset

local function offsetFile()
    return getUserProfileAppPath() .. "modSettings/FS25_VR.xml"
end

function VRMod:loadOffsets()
    self.offsets = {}
    local ok, err = pcall(function()
        local path = offsetFile()
        if not fileExists(path) then
            return
        end
        local xml = loadXMLFile("fs25vrSettings", path)
        if xml == nil or xml == 0 then
            return
        end
        VRMod.savedWorldScale = getXMLFloat(xml, "fs25vr#worldScale")
        VRMod.savedHudShown = getXMLBool(xml, "fs25vr#hudShown")
        VRMod.savedFoveated = getXMLBool(xml, "fs25vr#foveated")
        VRMod.savedSyncPhase = getXMLInt(xml, "fs25vr#syncPhase")
        local forceSAO = getXMLBool(xml, "fs25vr#forceSAO")
        if forceSAO ~= nil then
            VRMod.forceSAO = forceSAO
        end
        local depthMetres = getXMLFloat(xml, "fs25vr#hudDepthMetres")
        if depthMetres == nil then
            local oldIndex = getXMLInt(xml, "fs25vr#hudDepth")
            depthMetres = oldIndex ~= nil and VRMod.HUD_DEPTHS_V1[oldIndex] or nil
        end
        if depthMetres ~= nil then
            VRMod.hudDepthIndex = VRMod.findHudDepth(depthMetres)
        end
        local lowerIndex = getXMLInt(xml, "fs25vr#hudLower")
        if lowerIndex ~= nil and VRMod.HUD_LOWERS[lowerIndex] ~= nil then
            VRMod.hudLowerIndex = lowerIndex
        end
        local size = getXMLInt(xml, "fs25vr#hudSize")
        if size ~= nil and VRMod.HUD_SIZES[size] ~= nil then
            VRMod.hudSize = size
        end
        local i = 0
        while true do
            local key = string.format("fs25vr.offset(%d)", i)
            local name = getXMLString(xml, key .. "#name")
            if name == nil then
                break
            end
            self.offsets[name] = {getXMLFloat(xml, key .. "#x") or 0, getXMLFloat(xml, key .. "#y") or 0,
                getXMLFloat(xml, key .. "#z") or 0}
            i = i + 1
        end
        delete(xml)
    end)
    if not ok then
        log("could not read head offsets: %s", tostring(err))
    end
end

function VRMod:saveOffsets()
    self.offsetDirty = false
    local ok, err = pcall(function()
        createFolder(getUserProfileAppPath() .. "modSettings/")
        local xml = createXMLFile("fs25vrSettings", offsetFile(), "fs25vr")
        if xml == nil or xml == 0 then
            return
        end
        local names = {}
        for name, o in pairs(self.offsets) do
            if o[1] ~= 0 or o[2] ~= 0 or o[3] ~= 0 then
                table.insert(names, name)
            end
        end
        table.sort(names)
        for i, name in ipairs(names) do
            local o = self.offsets[name]
            local key = string.format("fs25vr.offset(%d)", i - 1)
            setXMLString(xml, key .. "#name", name)
            setXMLFloat(xml, key .. "#x", o[1])
            setXMLFloat(xml, key .. "#y", o[2])
            setXMLFloat(xml, key .. "#z", o[3])
        end
        setXMLInt(xml, "fs25vr#hudSize", VRMod.hudSize)
        setXMLBool(xml, "fs25vr#hudShown", not VRMod.hideHud)
        setXMLBool(xml, "fs25vr#forceSAO", VRMod.forceSAO == true)
        if self.api ~= nil then
            if self.api.getWorldScale ~= nil then
                setXMLFloat(xml, "fs25vr#worldScale", self.api.getWorldScale())
            end
            if self.api.getFoveated ~= nil then
                setXMLBool(xml, "fs25vr#foveated", self.api.getFoveated())
            end
            if self.api.getSyncPhase ~= nil then
                setXMLInt(xml, "fs25vr#syncPhase", self.api.getSyncPhase())
            end
        end
        if VRMod.hudLowerIndex ~= nil then
            setXMLInt(xml, "fs25vr#hudLower", VRMod.hudLowerIndex)
        end
        if VRMod.hudDepthIndex ~= nil then
            setXMLFloat(xml, "fs25vr#hudDepthMetres", VRMod.HUD_DEPTHS[VRMod.hudDepthIndex])
        end
        saveXMLFile(xml)
        delete(xml)
    end)
    if not ok then
        log("could not save head offsets: %s", tostring(err))
    end
end

function VRMod:showOffset(name, quiet)
    local o = self.offsets[name] or {0, 0, 0}
    local where = name == "foot" and "on foot" or "this cab"
    self:showMessage(string.format("Head offset (%s): right %+.2f  up %+.2f  forward %+.2f m",
        where, o[1], o[2], -o[3]), quiet)
end

function VRMod:updateOffset(dt)
    if next(self.offsetHeld) == nil then
        return
    end
    local name = self.activeOffsetKey
    if name == nil or not self:isStereoAllowed() then
        -- a menu opened (or an exterior camera): drop the keys so none stays stuck down
        self.offsetHeld = {}
        if self.offsetDirty then
            self:saveOffsets()
        end
        if name == nil then
            self:showMessage("Head offset: only in a cab or on foot", true)
        end
        return
    end
    local o = self.offsets[name]
    if o == nil then
        o = {0, 0, 0}
        self.offsets[name] = o
    end
    local step = VRMod.OFFSET_SPEED * dt / 1000
    local limit = VRMod.OFFSET_LIMIT
    for sym, _ in pairs(self.offsetHeld) do
        local axis = VRMod.OFFSET_KEYS[sym]
        o[axis[1]] = math.max(-limit, math.min(limit, o[axis[1]] + axis[2] * step))
    end
    self.offsetDirty = true
    self:showOffset(name, true)
end

---------------------------------------------------------------------------------------------------

function VRMod:update(dt)
    if self.api == nil then
        return
    end
    local running = self.api.isRunning()
    if running ~= self.wasRunning then
        self.wasRunning = running
        self:showMessage(running and "VR active" or "VR headset inactive")
        if not running then
            self:restoreFrameLimiter()
            self:restoreSSAO()
            self:restoreTemporalAA()
            self:restoreHudLayout()
        end
    end
    if running then
        -- The game's own frame cap fights the headset's pacing: a 60 fps cap under 90 Hz
        -- pacing makes every frame miss a refresh and lands on 45 fps. The headset paces the
        -- game while VR runs; re-applied periodically in case the game sets it again.
        self.limiterTimer = (self.limiterTimer or 0) - dt
        if self.limiterTimer <= 0 then
            self.limiterTimer = 2000
            setFramerateLimiter(false, 0)
            self.limiterOverridden = true
        end
        self:updateSSAO()
        self:updateTemporalAA()
        self:updateHudLayout(dt)
        self:updateHudShift()
        if not VRMod.hideHud then
            self:logHudDiagnostics(dt)
        end
        -- first VR session on this headset: the bridge measured the render size it needs, which
        -- the game only takes at start-up
        if not self.sizeNoteShown and self.api.renderSizeNote ~= nil then
            self.sizeNoteTimer = (self.sizeNoteTimer or 6000) - dt
            if self.sizeNoteTimer <= 0 then
                local note = self.api.renderSizeNote()
                if note ~= nil then
                    self.sizeNoteShown = true
                    self:showMessage(note, false, 12000)
                else
                    self.sizeNoteTimer = 5000
                end
            end
        end
    end
    self:updateHud(running and self:isStereoAllowed())
    self:updateOffset(dt)

    -- leaving stereo (menus, toggle): give the camera its normal projection back
    if not self:isStereoAllowed() then
        VRMod.hudShift = 0
        VRMod.hudSkip = false
        VRMod.crosshairDX, VRMod.crosshairDY = 0, 0
        for cam, s in pairs(self.cams) do
            if s.applied or s.projApplied then
                self:restoreCamera(cam, true)
            end
        end
    end
    if self.messageText ~= nil then
        self.messageTime = self.messageTime - dt
        if self.messageTime <= 0 then
            self.messageText = nil
        end
    end
end

function VRMod:draw()
    -- While the bridge calibrates its frame latency, stamp (frame mod 4) into the top-left pixels
    -- (red = bit 0, green = bit 1, blue = marker present). The bridge reads it back from the
    -- presented image. Lasts a second or two after VR starts.
    if self.markerFrame ~= nil then
        if self.markerOverlay ~= nil and self.markerOverlay ~= 0 and self.api.calibrating() then
            local f = math.floor(self.markerFrame) % 4
            setOverlayColor(self.markerOverlay, f % 2, math.floor(f / 2), 1, 1)
            local w, h = 8 / g_screenWidth, 8 / g_screenHeight
            local render = VRMod.renderOverlayRaw or renderOverlay  -- exact pixels, no HUD shift
            render(self.markerOverlay, 0, 1 - h, w, h)
        end
        self.markerFrame = nil
    end
    if self.messageText ~= nil then
        setTextAlignment(RenderText.ALIGN_CENTER)
        setTextBold(true)
        renderText(0.5, 0.6, 0.025, self.messageText)
        setTextBold(false)
        setTextAlignment(RenderText.ALIGN_LEFT)
    end
end

function VRMod:keyEvent(unicode, sym, modifier, isDown)
    if self.api == nil then
        return
    end
    if VRMod.OFFSET_KEYS[sym] ~= nil then
        self.offsetHeld[sym] = isDown or nil
        if next(self.offsetHeld) == nil and self.offsetDirty then
            self:saveOffsets()
            if self.activeOffsetKey ~= nil then
                self:showOffset(self.activeOffsetKey)
            end
        end
        return
    end
    if not isDown then
        return
    end
    if sym == VRMod.KEY_OFFSET_RESET then
        local name = self.activeOffsetKey
        if name ~= nil and self:isStereoAllowed() then
            self.offsets[name] = nil
            self:saveOffsets()
            self:showOffset(name)
        end
    elseif sym == VRMod.KEY_RECENTER then
        self.api.recenter()
        self:showMessage("VR view recentred")
    elseif sym == VRMod.KEY_FRUSTUM and self.api.setSymmetric ~= nil then
        VRMod.symmetric = not VRMod.symmetric
        self.api.setSymmetric(VRMod.symmetric)
        self:showMessage(VRMod.symmetric and "Projection: centred" or "Projection: off-centre (exact)")
    elseif sym == VRMod.KEY_SYNC_PHASE and self.api.setSyncPhase ~= nil then
        local phase = 1 - self.api.getSyncPhase()
        self:setSyncPhase(phase)
        self:saveOffsets()
        self:showMessage(string.format("Eye sync phase %d (keep the one where moving things look solid)", phase))
    elseif sym == VRMod.KEY_FOVEATED and self.api.setFoveated ~= nil then
        local on = not self.api.getFoveated()
        if self:setFoveated(on) then
            self:saveOffsets()
            self:showMessage(on and "Foveated rendering on" or "Foveated rendering off")
        else
            self:showMessage("Foveated rendering is not supported on this GPU")
        end
    elseif sym == VRMod.KEY_HUD and altDown() then
        VRMod.hudDepthIndex = (VRMod.hudDepthIndex or #VRMod.HUD_DEPTHS) % #VRMod.HUD_DEPTHS + 1
        self:applyHudSize()
        self:saveOffsets()
        local depth = VRMod.HUD_DEPTHS[VRMod.hudDepthIndex]
        self:showMessage(depth > 0 and string.format("HUD depth %.2f m", depth) or "HUD depth: infinity")
    elseif sym == VRMod.KEY_HUD and ctrlDown() then
        VRMod.hudLowerIndex = (VRMod.hudLowerIndex or 3) % #VRMod.HUD_LOWERS + 1
        self:applyHudSize()
        self:saveOffsets()
        self:showMessage(string.format("HUD height %d of %d (moved down %.2f)", VRMod.hudLowerIndex,
            #VRMod.HUD_LOWERS, VRMod.HUD_LOWERS[VRMod.hudLowerIndex]))
    elseif sym == VRMod.KEY_HUD and shiftDown() then
        VRMod.hudSize = VRMod.hudSize % #VRMod.HUD_SIZES + 1
        self:applyHudSize()
        self:saveOffsets()
        local preset = VRMod.HUD_SIZES[VRMod.hudSize]
        self:showMessage(string.format("HUD size %d of %d (area %.2f, scale %.1f)", VRMod.hudSize, #VRMod.HUD_SIZES,
            preset.area, preset.scale))
    elseif sym == VRMod.KEY_HUD then
        self:setHudShown(VRMod.hideHud)
        self:saveOffsets()
        self:showMessage(VRMod.hideHud and "HUD hidden in VR" or "HUD shown in VR")
    elseif sym == VRMod.KEY_SSAO then
        VRMod.forceSAO = not VRMod.forceSAO
        self:saveOffsets()
        self:showMessage(VRMod.forceSAO and "Ambient occlusion: SAO" or "Ambient occlusion: game setting")
    elseif sym == VRMod.KEY_TOGGLE then
        self.enabled = not self.enabled
        self:showMessage(self.enabled and "VR camera on" or "VR camera off (flat screen)")
    end
end

function VRMod:mouseEvent(posX, posY, isDown, isUp, button)
end

function VRMod:showMessage(text, quiet, ms)
    if not quiet then
        log("%s", text)
    end
    self.messageText = text
    self.messageTime = ms or 2500
end

---------------------------------------------------------------------------------------------------
-- Settings shared by the keys and the VR section of the game's settings page. Saved in
-- modSettings/FS25_VR.xml together with the head offsets.

VRMod.WORLD_SCALES = {0.85, 0.9, 0.95, 1.0, 1.05, 1.1, 1.15}

local function nearestIndex(list, value)
    local best, bestDist = 1, math.huge
    for i, v in ipairs(list) do
        if math.abs(v - value) < bestDist then
            best, bestDist = i, math.abs(v - value)
        end
    end
    return best
end

function VRMod:setHudShown(shown)
    VRMod.hideHud = not shown
    local hud = g_currentMission ~= nil and g_currentMission.hud or nil
    if shown and hud ~= nil and hud.setIsVisible ~= nil then
        -- show it whoever hid it (the game itself, or a hide during loading)
        hud:setIsVisible(true)
        self.hudHidden = false
    end
    self.hudDiagTimer = 0  -- report the HUD state right away
end

function VRMod:setFoveated(on)
    return self.api.setFoveated ~= nil and self.api.setFoveated(on)
end

function VRMod:setSyncPhase(phase)
    if self.api.setSyncPhase ~= nil then
        self.api.setSyncPhase(phase)
    end
end

function VRMod:setWorldScale(scale)
    if self.api.setWorldScale ~= nil then
        self.api.setWorldScale(scale)
        VRMod.worldScale = scale
    end
end

-- Puts the saved settings into effect (loadMap, once the bridge is connected).
function VRMod:applySavedSettings()
    if VRMod.savedHudShown ~= nil then
        VRMod.hideHud = not VRMod.savedHudShown
    end
    if VRMod.savedFoveated ~= nil then
        self:setFoveated(VRMod.savedFoveated)
    end
    if VRMod.savedSyncPhase ~= nil then
        self:setSyncPhase(VRMod.savedSyncPhase)
    end
    if VRMod.savedWorldScale ~= nil then
        self:setWorldScale(VRMod.savedWorldScale)
    end
end

-- One entry per row of the VR settings section: texts, current state (index) and what a choice does.
function VRMod:getSettingDefs()
    local function formatList(list, fmt, zeroText)
        local t = {}
        for _, v in ipairs(list) do
            table.insert(t, (v == 0 and zeroText) or string.format(fmt, v))
        end
        return t
    end
    return {
        {title = "VR: world scale", tip = "Above 1 the world feels smaller, below 1 bigger.",
            texts = formatList(VRMod.WORLD_SCALES, "%.2f"),
            get = function()
                return nearestIndex(VRMod.WORLD_SCALES, self.api.getWorldScale ~= nil and self.api.getWorldScale() or 1)
            end,
            set = function(i) self:setWorldScale(VRMod.WORLD_SCALES[i]) end},
        {title = "VR: HUD", tip = "Show the HUD in the headset (F10).", texts = {"Hidden", "Shown"},
            get = function() return VRMod.hideHud and 1 or 2 end,
            set = function(i) self:setHudShown(i == 2) end},
        {title = "VR: HUD size", tip = "How much of the view the HUD uses, and its size (Shift+F10).",
            texts = {"Large", "Medium", "Small", "Smallest"},
            get = function() return VRMod.hudSize end,
            set = function(i) VRMod.hudSize = i; self:applyHudSize() end},
        {title = "VR: HUD height", tip = "Moves the whole HUD down (Ctrl+F10).",
            texts = formatList(VRMod.HUD_LOWERS, "%.2f lower", "Centre"),
            get = function() return VRMod.hudLowerIndex or 3 end,
            set = function(i) VRMod.hudLowerIndex = i; self:applyHudSize() end},
        {title = "VR: HUD depth", tip = "Distance the HUD appears at: pick the one where it does not look double (Alt+F10).",
            texts = formatList(VRMod.HUD_DEPTHS, "%.2f m", "Infinity"),
            get = function() return VRMod.hudDepthIndex or #VRMod.HUD_DEPTHS end,
            set = function(i) VRMod.hudDepthIndex = i; self:applyHudSize() end},
        {title = "VR: foveated rendering", tip = "Renders the edges of the view with less detail for more frame rate (F5).",
            texts = {"Off", "On"},
            get = function() return (self.api.getFoveated ~= nil and self.api.getFoveated()) and 2 or 1 end,
            set = function(i) self:setFoveated(i == 2) end},
        {title = "VR: eye sync phase", tip = "If moving things look doubled or lose depth, try the other one (F3).",
            texts = {"0", "1"},
            get = function() return (self.api.getSyncPhase ~= nil and self.api.getSyncPhase() or 0) + 1 end,
            set = function(i) self:setSyncPhase(i - 1) end},
        {title = "VR: ambient occlusion", tip = "Game setting, or force the simple SAO (F7).",
            texts = {"Game setting", "Force SAO"},
            get = function() return VRMod.forceSAO and 2 or 1 end,
            set = function(i) VRMod.forceSAO = i == 2 end},
    }
end

-- The VR section is built from copies of the settings page's own rows (a section header and a
-- multi-choice row), so it looks and behaves like the game's settings. If the page is laid out
-- differently than expected nothing is added and the keys still work.
function VRMod:addSettingsSection()
    if self.settingsRows ~= nil then
        return
    end
    local page = g_inGameMenu ~= nil and g_inGameMenu.pageSettings or nil
    if page == nil or page.getDescendants == nil then
        log("settings menu: settings page not found; VR settings only on the keys")
        return
    end
    local function isOption(e) return e:isa(MultiTextOptionElement) end
    -- the scrolling list with the most option rows (the general settings)
    local list, rows = nil, 0
    for _, layout in ipairs(page:getDescendants(function(e) return e:isa(ScrollingLayoutElement) end)) do
        local n = 0
        for _, row in ipairs(layout.elements) do
            if row:getFirstDescendant(isOption) ~= nil then
                n = n + 1
            end
        end
        if n > rows then
            list, rows = layout, n
        end
    end
    if list == nil then
        log("settings menu: no option list found on the settings page")
        return
    end
    local rowTemplate, headerTemplate
    for _, row in ipairs(list.elements) do
        local opt = row:getFirstDescendant(isOption)
        if opt ~= nil and rowTemplate == nil and not opt:isa(BinaryOptionElement) then
            rowTemplate = row
        elseif opt == nil and headerTemplate == nil and row:isa(TextElement) then
            headerTemplate = row
        end
    end
    if rowTemplate == nil then
        log("settings menu: no multi-choice row to copy")
        return
    end

    if headerTemplate ~= nil then
        local header = headerTemplate:clone(list)
        header:setText("Virtual reality (FS25_VR)")
    end
    self.settingsRows = {}
    for _, def in ipairs(self:getSettingDefs()) do
        local row = rowTemplate:clone(list)
        local opt = row:getFirstDescendant(isOption)
        -- the row's own texts: the title, then the description
        local texts = {}
        for _, e in ipairs(row.elements) do
            if e ~= opt and e:isa(TextElement) then
                table.insert(texts, e)
            end
        end
        if texts[1] ~= nil then texts[1]:setText(def.title) end
        if texts[2] ~= nil then texts[2]:setText(def.tip) end
        opt.toolTipText = def.tip
        opt:setTexts(def.texts)
        opt:setState(def.get())
        opt.onClickCallback = function(a, b)
            local state = type(a) == "number" and a or b  -- called with the page as target, or without
            local ok, err = pcall(def.set, state)
            if not ok then
                log("setting '%s' failed: %s", def.title, tostring(err))
            end
            self:saveOffsets()
        end
        table.insert(self.settingsRows, {opt = opt, def = def})
    end
    list:invalidateLayout()

    -- show the current state whenever the page opens (the keys may have changed it)
    local onFrameOpen = page.onFrameOpen
    page.onFrameOpen = function(p, ...)
        if onFrameOpen ~= nil then
            onFrameOpen(p, ...)
        end
        VRMod:refreshSettingsRows()
    end
    log("settings menu: VR section added (%d settings)", #self.settingsRows)
end

function VRMod:refreshSettingsRows()
    for _, r in ipairs(self.settingsRows or {}) do
        pcall(function() r.opt:setState(r.def.get()) end)
    end
end

addModEventListener(VRMod)
