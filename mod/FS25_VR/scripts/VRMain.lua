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
-- quad views: plane stereo plus a sharper focus view per eye (chosen in the settings)
VRMod.quadViews = false
-- F11: VR settings (stereo mode: alternating eyes, plane stereo, quad views; 3D resolution; arrange
-- the HUD). Shift+F10: arrange the HUD panels.
VRMod.KEY_SETTINGS = Input.KEY_f11 or 292
VRMod.STEREO_RETRY = 10000           -- ms between automatic plane stereo starts after a failure
-- The bridge shows the HUD as panels in the cab (F10 hides it). With hudPanel=0 in fs25vr.ini it
-- would be drawn flat into each eye's image, in the corners of your vision at screen depth: hidden
-- there instead (F10 shows it again).
VRMod.hideHud = true
VRMod.hidePanel = false
VRMod.symmetric = true

-- Ambient occlusion: quality 1 ("Low") is SAO, a purely spatial pass; quality 2 and up is XeGTAO.
-- With the bridge's centred per-eye projection both are consistent between the eyes (the AO
-- shaders assume a centred frustum), so the player's setting is kept. F7 can force SAO.
VRMod.SSAO_SAO = 1
VRMod.forceSAO = false

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

    self.hudPanels = VRHud.new(api)
    self:installHooks()
    self:loadOffsets()
    if self.settings.renderScale ~= nil and set3dResolutionScaling ~= nil then
        set3dResolutionScaling(self.settings.renderScale)
    end
    local okGui, err = pcall(function()
        local dialog = VRSettingsDialog.new()
        dialog.mod = self
        g_gui:loadGui(self.modDirectory .. "gui/VRSettingsDialog.xml", "VRSettingsDialog", dialog)
    end)
    if not okGui then
        log("settings menu not available: %s", tostring(err))
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

function VRMod:updateHud(inVr)
    local hud = g_currentMission ~= nil and g_currentMission.hud or nil
    if hud == nil or hud.setIsVisible == nil then
        return
    end
    local hide = VRMod.hideHud
    if self:hudPanelShown() then
        hide = VRMod.hidePanel
    end
    if inVr and hide then
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

-- The bridge shows the HUD as panels in the 3D view (hudPanel=1 in fs25vr.ini)
function VRMod:hudPanelShown()
    return self.api ~= nil and self.api.hudPanel ~= nil and self.api.hudPanel()
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
    self:updateHud(false)
    if self.hudPanels ~= nil then
        self.hudPanels:reset()
    end
    self:stopPlaneStereo()
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
        local parent = getParent(posNode)
        if parent == 0 then
            return nil
        end
        local node = createTransformGroup("vrSeatNode")
        link(parent, node)
        local tx, ty, tz = localToLocal(posNode, parent, 0, 0, 0)
        local dx, _, dz = localDirectionToLocal(posNode, parent, 0, 0, 1)
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
    self.settings = {stereoMode = VRSettingsDialog.MODE_ALTERNATING}
    local ok, err = pcall(function()
        local path = offsetFile()
        if not fileExists(path) then
            return
        end
        local xml = loadXMLFile("fs25vrSettings", path)
        if xml == nil or xml == 0 then
            return
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
        local mode = getXMLInt(xml, "fs25vr.settings#stereoMode")
        if mode ~= nil and mode >= 1 and mode <= 3 then
            self.settings.stereoMode = mode
        end
        self.settings.renderScale = getXMLFloat(xml, "fs25vr.settings#renderScale")
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
        setXMLInt(xml, "fs25vr.settings#stereoMode", self.settings.stereoMode)
        if self.settings.renderScale ~= nil then
            setXMLFloat(xml, "fs25vr.settings#renderScale", self.settings.renderScale)
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
            self:stopPlaneStereo()
            self:restoreFrameLimiter()
            self:restoreSSAO()
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
    end
    self:updateHud(running and self:isStereoAllowed())
    if self.hudPanels ~= nil then
        local shown = running and self:isStereoAllowed() and self:hudPanelShown() and not VRMod.hidePanel
        if not shown and self.hudPanels.editing then
            self.hudPanels:setEditing(false)
        end
        self.hudPanels:update(dt, shown)
    end
    self:updateOffset(dt)
    self:updatePlaneStereo(dt)
    -- the stereo mode chosen in the settings: plane stereo starts by itself once VR shows the 3D view
    -- and the bridge has measured the frame latency (it needs alternating eyes for that)
    self.stereoRetry = math.max(0, (self.stereoRetry or 0) - dt)
    if running and self.planeStereo == nil and self.stereoRetry == 0 and
        self.settings.stereoMode ~= VRSettingsDialog.MODE_ALTERNATING and self:isStereoAllowed() and
        not self.api.calibrating() then
        self.stereoRetry = VRMod.STEREO_RETRY
        VRMod.quadViews = self.settings.stereoMode == VRSettingsDialog.MODE_QUAD
        self:startPlaneStereo()
    end

    -- leaving stereo (menus, toggle): give the camera its normal projection back
    if not self:isStereoAllowed() then
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
            if self.hudPanels ~= nil then
                self.hudPanels:setMessage(false)
            end
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
            renderOverlay(self.markerOverlay, 0, 1 - h, w, h)
        end
        self.markerFrame = nil
    end
    if self.hudPanels ~= nil then
        self.hudPanels:draw(self.messageText)
    elseif self.messageText ~= nil then
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
    if self.hudPanels ~= nil and self.hudPanels:keyEvent(sym, isDown) then
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
    elseif sym == VRMod.KEY_SETTINGS then
        if g_gui ~= nil and not g_gui:getIsGuiVisible() and g_gui.guis["VRSettingsDialog"] ~= nil then
            g_gui:showDialog("VRSettingsDialog")
        end
    elseif sym == VRMod.KEY_HUD and self.hudPanels ~= nil and self.hudPanels:isDown(Input.KEY_lshift, Input.KEY_rshift) then
        self:toggleHudArranging()
    elseif sym == VRMod.KEY_HUD and self.hudPanels ~= nil and self.hudPanels.editing then
        self.hudPanels:toggleShowHelp()
    elseif sym == VRMod.KEY_HUD then
        if self:hudPanelShown() then
            VRMod.hidePanel = not VRMod.hidePanel
            self:showMessage(VRMod.hidePanel and "HUD panel hidden" or "HUD panel shown")
        else
            VRMod.hideHud = not VRMod.hideHud
            self:showMessage(VRMod.hideHud and "HUD hidden in VR" or "HUD shown in VR")
        end
    elseif sym == VRMod.KEY_SSAO then
        VRMod.forceSAO = not VRMod.forceSAO
        self:showMessage(VRMod.forceSAO and "Ambient occlusion: SAO" or "Ambient occlusion: game setting")
    elseif sym == VRMod.KEY_TOGGLE then
        self.enabled = not self.enabled
        self:showMessage(self.enabled and "VR camera on" or "VR camera off (flat screen)")
    end
end

---------------------------------------------------------------------------------------------------
-- plane stereo: both eyes every frame (no alternating eyes, no frozen simulation). The engine
-- renders the right eye as a second view of its main render path, with full detail, shadows and
-- mirrors, into the output texture of a render overlay created here. The overlay renders by
-- itself for a few frames first, so the bridge can find that texture by its size; then the
-- bridge takes it over as the second view's output.

VRMod.PLANE_STEREO_WARMUP = 3        -- frames the overlay renders by itself
VRMod.PLANE_STEREO_TIMEOUT = 5000    -- ms until plane stereo is given up

local function bestAtmosphereQuality()
    local best = 0
    if AtmosphereQuality ~= nil then
        for _, v in pairs(AtmosphereQuality) do
            if type(v) == "number" and v > best then
                best = v
            end
        end
    end
    return best
end

function VRMod:getStereoMode()
    return self.settings.stereoMode
end

-- the game's 3D resolution scaling: every view renders at the window's size times it
function VRMod:getRenderScale()
    if self.settings.renderScale ~= nil then
        return self.settings.renderScale
    end
    return get3dResolutionScaling ~= nil and get3dResolutionScaling() or 1
end

-- from the settings menu
function VRMod:applySettings(mode, scale)
    local scaleChanged = math.abs(scale - self:getRenderScale()) > 0.001
    self.settings.stereoMode = mode
    self.settings.renderScale = scale
    self:saveOffsets()
    if scaleChanged and set3dResolutionScaling ~= nil then
        set3dResolutionScaling(scale)
    end
    local quad = mode == VRSettingsDialog.MODE_QUAD
    if mode == VRSettingsDialog.MODE_ALTERNATING then
        if self.planeStereo ~= nil then
            self:stopPlaneStereo()
            self:showMessage("Stereo: alternating eyes")
        end
    elseif self.planeStereo ~= nil and (quad ~= VRMod.quadViews or scaleChanged) then
        -- the views' render targets change size: new overlays
        VRMod.quadViews = quad
        self:stopPlaneStereo()
        self:startPlaneStereo()
    end
    -- otherwise plane stereo starts by itself (VRMod:update)
    VRMod.quadViews = quad
    self.stereoRetry = 0
end

function VRMod:startPlaneStereo()
    if self.api.prepareOverlay == nil or self.api.setPlaneStereo == nil then
        self:showMessage("Plane stereo: bridge too old")
        return
    end
    local sizes = {self.api.prepareOverlay(true, VRMod.quadViews, self:getRenderScale())}
    if sizes[1] == nil or sizes[1] == 0 then
        self:showMessage("Plane stereo: VR not running")
        return
    end
    if VRMod.quadViews and #sizes < 6 then
        VRMod.quadViews = false
        self:showMessage("Quad views unavailable (see log)", true)
    end
    local t = {frames = 0, time = 0, overlays = {}, cameras = {}, quad = #sizes >= 6}
    self.planeStereo = t
    -- in the order the bridge expects: the right eye, then the left and right focus views
    for i = 1, #sizes, 2 do
        local w, h = sizes[i], sizes[i + 1]
        local camera = createCamera("vrView" .. i, math.rad(60), 0.1, 1000)
        link(getRootNode(), camera)
        table.insert(t.cameras, camera)
        -- root 0: the engine's whole scene; all object and light mask bits
        local ok, ov = pcall(createRenderOverlay, 0, camera, w / h, w, h, false, -1, -1, true, 1, false,
            getSSAOQuality and getSSAOQuality() or 1, false, bestAtmosphereQuality())
        log("plane stereo: createRenderOverlay(%dx%d) -> %s %s", w, h, tostring(ok), tostring(ov))
        if not ok or ov == nil or ov == 0 then
            self:stopPlaneStereo()
            self:showMessage("Plane stereo: createRenderOverlay failed (see log)")
            return
        end
        table.insert(t.overlays, ov)
    end
end

function VRMod:stopPlaneStereo()
    local t = self.planeStereo
    if t == nil then
        return
    end
    self.planeStereo = nil
    if self.api ~= nil and self.api.prepareOverlay ~= nil then
        self.api.prepareOverlay(false)  -- also ends plane stereo in the bridge
    end
    for _, ov in ipairs(t.overlays) do
        delete(ov)
    end
    for _, camera in ipairs(t.cameras) do
        if entityExists(camera) then
            delete(camera)
        end
    end
end

-- While starting: has the overlays render by themselves, then hands them to the bridge.
function VRMod:updatePlaneStereo(dt)
    local t = self.planeStereo
    if t == nil or t.active then
        return
    end
    t.time = t.time + dt
    if t.time > VRMod.PLANE_STEREO_TIMEOUT then
        self:stopPlaneStereo()
        self:showMessage("Plane stereo: the render overlay did not render (see log)")
        return
    end
    if getIsOverlayReady ~= nil then
        for _, ov in ipairs(t.overlays) do
            if not getIsOverlayReady(ov) then
                return
            end
        end
    end
    -- queued once more at the end: the bridge takes them from the renderer's queue as the outputs
    -- of the engine's further views; from then on the overlays are not rendered by themselves
    for _, ov in ipairs(t.overlays) do
        updateRenderOverlay(ov)
    end
    t.frames = t.frames + 1
    if t.frames <= VRMod.PLANE_STEREO_WARMUP then
        return
    end
    if self.api.setPlaneStereo(true) then
        t.active = true
        self:showMessage(t.quad and "Stereo: plane stereo with quad views" or "Stereo: both eyes every frame (plane stereo)")
    else
        self:stopPlaneStereo()
        self:showMessage("Plane stereo unavailable (see log)")
    end
end

function VRMod:mouseEvent(posX, posY, isDown, isUp, button)
    if self.hudPanels ~= nil then
        self.hudPanels:mouseEvent(posX, posY, isDown, isUp, button)
    end
end

-- Shift+F10 / F11 menu: arrange the HUD panels (needs the panels: both eyes every frame)
function VRMod:toggleHudArranging()
    if self.hudPanels == nil then
        return
    end
    if self.hudPanels.editing then
        self.hudPanels:setEditing(false)
        self:showMessage("HUD layout saved")
    elseif not self:hudPanelShown() then
        self:showMessage("HUD panels are off (hudPanel in fs25vr.ini)")
    elseif VRMod.hidePanel then
        self:showMessage("Show the HUD first (F10)")
    elseif not (self.api.isRunning() and self:isStereoAllowed()) then
        self:showMessage("Arrange the HUD in the 3D view in VR")
    else
        self.hudPanels:setEditing(true)
        self:showMessage("Arranging the HUD (Shift+F10: done)")
    end
end

function VRMod:showMessage(text, quiet)
    if not quiet then
        log("%s", text)
    end
    self.messageText = text
    self.messageTime = 2500
    if self.hudPanels ~= nil then
        self.hudPanels:setMessage(true)
    end
end

addModEventListener(VRMod)
