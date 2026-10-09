-- HUD panels (plane stereo). The bridge takes the game's HUD out of the eyes' images and shows parts
-- of it as panels in the cab. This finds those parts: once a second, for one frame, it records where
-- the HUD draws (renderText, renderOverlay, drawFilledRect) and who draws it:
--   * everything drawn inside one of the HUD's displays (speed meter, map, game info, ...) is that
--     display's part, including what mods add to it;
--   * everything else belongs to the mod whose code made the call (found through the environments
--     on the call stack), split into blocks of nearby calls. A mod's block next to a display joins
--     that display's panel (e.g. extra gauges around the speed meter).
-- Parts only grow (things shown now and then keep their place) and start over in another vehicle.
-- Panels start where their part is on the flat HUD, placed on a plane in front of the seat.
--
-- Arranging (Shift+F10 or the F11 menu): every panel is outlined, the one under the crosshair is
-- highlighted. Hold the left mouse button: the panel (or the selection) follows your view; release to
-- place it. Shift+click: add to / remove from the selection; G: group the selection (grouped panels
-- are always picked together) or ungroup it. Wheel: distance; Shift+wheel: size; Ctrl+wheel: tilt;
-- Alt+wheel: turn; right click: back to the default place. F10 shows / hides the help. Saved per
-- vehicle (and on foot) in modSettings/FS25_VR_hud.xml.

VRHud = {}
local VRHud_mt = {__index = VRHud}

VRHud.SURVEY_INTERVAL = 1000  -- ms between recorded frames
VRHud.GAP = 0.015             -- screen units: calls of one mod closer than this form one block
VRHud.ATTACH = 0.015          -- a mod's block this close to a display joins the display's panel
VRHud.MAX_AREA = 0.5          -- larger parts are full-screen effects (fades), not panels
VRHud.MAX_PANELS = 20           -- parts (the bridge takes 24: + the help and the crosshair)
-- where the help and the mod's messages are drawn on the screen: places the game's HUD leaves free
-- (the help: left of the game info, above where the largest map reaches); both become panels that
-- move with the head
VRHud.HELP = {x0 = 0.27, y0 = 0.62, x1 = 0.68, y1 = 0.88}
VRHud.MESSAGE = {x0 = 0.15, y0 = 0.565, x1 = 0.85, y1 = 0.625}
VRHud.STEP = 1.05             -- wheel: distance and size factor per notch
VRHud.ANGLE_STEP = math.rad(3)
VRHud.MIN_DISTANCE = 0.2

-- quaternions {x, y, z, w}
local function qmul(a, b)
    return {a[4] * b[1] + a[1] * b[4] + a[2] * b[3] - a[3] * b[2],
            a[4] * b[2] - a[1] * b[3] + a[2] * b[4] + a[3] * b[1],
            a[4] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[4],
            a[4] * b[4] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3]}
end

local function qrot(q, v)
    local r = qmul(qmul(q, {v[1], v[2], v[3], 0}), {-q[1], -q[2], -q[3], q[4]})
    return {r[1], r[2], r[3]}
end

local function dot(a, b)
    return a[1] * b[1] + a[2] * b[2] + a[3] * b[3]
end

-- a panel's orientation: yaw about +y, then pitch about its own x axis (as the bridge builds it)
local function panelRotation(yaw, pitch)
    return qmul({0, math.sin(0.5 * yaw), 0, math.cos(0.5 * yaw)}, {math.sin(0.5 * pitch), 0, 0, math.cos(0.5 * pitch)})
end

local function layoutFile()
    return getUserProfileAppPath() .. "modSettings/FS25_VR_hud.xml"
end

local function rectGap(a, b)
    return math.max(a.x0 - b.x1, b.x0 - a.x1, a.y0 - b.y1, b.y0 - a.y1)
end

local function unite(a, b)
    a.x0 = math.min(a.x0, b.x0)
    a.y0 = math.min(a.y0, b.y0)
    a.x1 = math.max(a.x1, b.x1)
    a.y1 = math.max(a.y1, b.y1)
end

-- the table holding the engine's global functions (the environments only look them up)
local function findEngineGlobals()
    local t = getfenv(1)
    for _ = 1, 8 do
        if rawget(t, "renderText") ~= nil then
            return t
        end
        local mt = getmetatable(t)
        if mt == nil or type(mt.__index) ~= "table" then
            return nil
        end
        t = mt.__index
    end
    return nil
end

function VRHud.new(api)
    local self = setmetatable({}, VRHud_mt)
    self.api = api
    self.parts = {}      -- key -> {x0, y0, x1, y1} in screen units (y up)
    self.timer = 0
    self.surveying = false
    self.sent = false
    self.panels = {}     -- what was sent: key, x, y, z, yaw, pitch, width, height
    self.layouts = {}    -- layout id (vehicle / on foot) -> part key -> {x, y, z, yaw, pitch, width}
    self.layoutId = "onFoot"
    self.editing = false
    self.selected = {}
    self.showHelp = true
    self:loadLayouts()
    return self
end

function VRHud:loadLayouts()
    local ok, err = pcall(function()
        if not fileExists(layoutFile()) then
            return
        end
        local xml = loadXMLFile("fs25vrHud", layoutFile())
        if xml == nil or xml == 0 then
            return
        end
        local i = 0
        while true do
            local lkey = string.format("hud.layout(%d)", i)
            local id = getXMLString(xml, lkey .. "#id")
            if id == nil then
                break
            end
            local layout = {}
            local j = 0
            while true do
                local pkey = string.format("%s.panel(%d)", lkey, j)
                local key = getXMLString(xml, pkey .. "#key")
                if key == nil then
                    break
                end
                layout[key] = {getXMLFloat(xml, pkey .. "#x") or 0, getXMLFloat(xml, pkey .. "#y") or 0,
                    getXMLFloat(xml, pkey .. "#z") or -1, getXMLFloat(xml, pkey .. "#yaw") or 0,
                    getXMLFloat(xml, pkey .. "#pitch") or 0, getXMLFloat(xml, pkey .. "#width") or 0.1,
                    getXMLInt(xml, pkey .. "#group")}
                j = j + 1
            end
            self.layouts[id] = layout
            i = i + 1
        end
        delete(xml)
    end)
    if not ok then
        print("FS25_VR: could not read HUD layouts: " .. tostring(err))
    end
end

function VRHud:saveLayouts()
    local ok, err = pcall(function()
        createFolder(getUserProfileAppPath() .. "modSettings/")
        local xml = createXMLFile("fs25vrHud", layoutFile(), "hud")
        local ids = {}
        for id, _ in pairs(self.layouts) do
            table.insert(ids, id)
        end
        table.sort(ids)
        local i = 0
        for _, id in ipairs(ids) do
            local keys = {}
            for key, _ in pairs(self.layouts[id]) do
                table.insert(keys, key)
            end
            if #keys > 0 then
                table.sort(keys)
                local lkey = string.format("hud.layout(%d)", i)
                setXMLString(xml, lkey .. "#id", id)
                for j, key in ipairs(keys) do
                    local p = self.layouts[id][key]
                    local pkey = string.format("%s.panel(%d)", lkey, j - 1)
                    setXMLString(xml, pkey .. "#key", key)
                    setXMLFloat(xml, pkey .. "#x", p[1])
                    setXMLFloat(xml, pkey .. "#y", p[2])
                    setXMLFloat(xml, pkey .. "#z", p[3])
                    setXMLFloat(xml, pkey .. "#yaw", p[4])
                    setXMLFloat(xml, pkey .. "#pitch", p[5])
                    setXMLFloat(xml, pkey .. "#width", p[6])
                    if p[7] ~= nil then
                        setXMLInt(xml, pkey .. "#group", p[7])
                    end
                end
                i = i + 1
            end
        end
        saveXMLFile(xml)
        delete(xml)
    end)
    if not ok then
        print("FS25_VR: could not save HUD layouts: " .. tostring(err))
    end
end

function VRHud:reset()
    self.parts = {}
    self.panels = {}
    self.sent = false
    if self.api.setHudPanelCount ~= nil then
        self.api.setHudPanelCount(-1)
    end
end

-- per frame; active: the bridge shows the HUD as panels
function VRHud:update(dt, active)
    if self.surveying then
        self:endSurvey()
    end
    if not active or self.api.setHudPanel == nil then
        return
    end
    local vehicle = self:currentVehicle()
    if vehicle ~= self.vehicle then
        self.vehicle = vehicle
        self.layoutId = vehicle ~= nil and ("cab:" .. tostring(vehicle.configFileName)) or "onFoot"
        self:reset()
        self.timer = 0
    end
    if self.editing then
        self:updateEditing()
    end
    self.timer = self.timer - dt
    if self.timer <= 0 then
        self.timer = VRHud.SURVEY_INTERVAL
        self:beginSurvey()
    end
end

function VRHud:currentVehicle()
    local ok, v = pcall(function()
        return g_localPlayer ~= nil and g_localPlayer:getCurrentVehicle() or nil
    end)
    return ok and v or nil
end

-- the mod whose code is on the call stack (the first environment that is another mod's), else "game"
-- (our own frames, the wrappers, are skipped: their environment is this mod's)
function VRHud:callerMod()
    for level = 2, 14 do
        local ok, env = pcall(getfenv, level)
        if not ok or env == nil then
            break
        end
        if env ~= self.ownEnv then
            local name = self.envName[env]
            if name ~= nil then
                return name
            end
        end
    end
    return "game"
end

function VRHud:record(x0, y0, x1, y1)
    if self.drawingHelp then
        return
    end
    -- reaching well beyond the screen: a texture shown through a mask (the map), not the display's extent
    local beyond = x0 < -0.02 or y0 < -0.02 or x1 > 1.02 or y1 > 1.02
    x0, x1 = math.max(x0, 0), math.min(x1, 1)
    y0, y1 = math.max(y0, 0), math.min(y1, 1)
    if x1 <= x0 or y1 <= y0 then
        return
    end
    local display = self.display
    table.insert(self.calls, {display = display, owner = display == nil and self:callerMod() or nil,
                              beyond = beyond, x0 = x0, y0 = y0, x1 = x1, y1 = y1})
end

function VRHud:textRect(x, y, size, text)
    text = tostring(text)
    local w = getTextWidth ~= nil and getTextWidth(size, text) or 0
    local lines = 1
    for _ in string.gmatch(text, "\n") do
        lines = lines + 1
    end
    local rt = RenderText or {}
    local x0 = x
    if self.align == rt.ALIGN_CENTER then
        x0 = x - 0.5 * w
    elseif self.align == rt.ALIGN_RIGHT then
        x0 = x - w
    end
    local h = size * lines
    local y0, y1
    if self.valign == rt.VERTICAL_ALIGN_TOP then
        y0, y1 = y - h, y
    elseif self.valign == rt.VERTICAL_ALIGN_MIDDLE then
        y0, y1 = y - 0.5 * h, y + 0.5 * h
    elseif self.valign == rt.VERTICAL_ALIGN_BOTTOM then
        y0, y1 = y, y + h
    else  -- baseline: further lines go down, descenders below the baseline
        y0, y1 = y - size * (lines - 1) - 0.25 * size, y + size
    end
    return x0, y0, x0 + w, y1
end

function VRHud:beginSurvey()
    self.engine = self.engine or findEngineGlobals()
    if self.engine == nil then
        return
    end
    if self.envName == nil then
        self.ownEnv = getfenv(1)
        self.envName = {}
        if g_modManager ~= nil then
            for _, m in ipairs(g_modManager:getActiveMods()) do
                if type(_G[m.modName]) == "table" then
                    self.envName[_G[m.modName]] = m.modName
                end
            end
        end
    end
    local hud = g_currentMission ~= nil and g_currentMission.hud or nil
    if hud == nil then
        return
    end
    self.calls = {}
    self.restore = {}
    self.align = RenderText ~= nil and RenderText.ALIGN_LEFT or nil
    self.valign = nil
    local this = self
    local engine = self.engine
    local function wrap(name, wrapper)
        local orig = rawget(engine, name)
        if orig ~= nil and pcall(rawset, engine, name, wrapper(orig)) then
            table.insert(this.restore, function() rawset(engine, name, orig) end)
        end
    end
    wrap("renderText", function(orig)
        return function(x, y, size, text, ...)
            local ok, x0, y0, x1, y1 = pcall(this.textRect, this, x, y, size, text)
            if ok then
                this:record(x0, y0, x1, y1)
            end
            return orig(x, y, size, text, ...)
        end
    end)
    wrap("renderOverlay", function(orig)
        return function(id, x, y, w, h, ...)
            if type(x) == "number" and type(w) == "number" then
                this:record(x, y, x + w, y + h)
            end
            return orig(id, x, y, w, h, ...)
        end
    end)
    wrap("drawFilledRect", function(orig)
        return function(x, y, w, h, ...)
            if type(x) == "number" and type(w) == "number" then
                this:record(x, y, x + w, y + h)
            end
            return orig(x, y, w, h, ...)
        end
    end)
    wrap("setTextAlignment", function(orig)
        return function(a, ...)
            this.align = a
            return orig(a, ...)
        end
    end)
    wrap("setTextVerticalAlignment", function(orig)
        return function(a, ...)
            this.valign = a
            return orig(a, ...)
        end
    end)
    -- what a display draws belongs to it
    for name, display in pairs(hud) do
        if type(display) == "table" and type(display.draw) == "function" then
            local own = rawget(display, "draw")
            local draw = display.draw
            display.draw = function(...)
                local prev = this.display
                this.display = name
                draw(...)
                this.display = prev
            end
            table.insert(this.restore, function() display.draw = own end)
        end
    end
    self.display = nil
    self.surveying = true
end

function VRHud:endSurvey()
    self.surveying = false
    for i = #self.restore, 1, -1 do
        self.restore[i]()
    end
    self.restore = {}

    -- displays: the union of their calls (without those reaching beyond the screen, unless that is all
    -- a display draws)
    local blocks = {}
    local byDisplay = {}
    local loose = {}  -- owner -> list of rects
    local onScreen = {}  -- displays with calls that stay on the screen
    for _, c in ipairs(self.calls) do
        if c.display ~= nil and not c.beyond then
            onScreen[c.display] = true
        end
    end
    for _, c in ipairs(self.calls) do
        if c.display ~= nil then
            if not c.beyond or not onScreen[c.display] then
                local b = byDisplay[c.display]
                if b == nil then
                    b = {key = "hud." .. c.display, x0 = c.x0, y0 = c.y0, x1 = c.x1, y1 = c.y1}
                    byDisplay[c.display] = b
                    table.insert(blocks, b)
                else
                    unite(b, c)
                end
            end
        else
            loose[c.owner] = loose[c.owner] or {}
            table.insert(loose[c.owner], {x0 = c.x0, y0 = c.y0, x1 = c.x1, y1 = c.y1})
        end
    end
    local displayBlocks = {}
    for _, b in ipairs(blocks) do
        table.insert(displayBlocks, b)
    end

    -- everything else: blocks of nearby calls per owner; next to a display: part of its panel
    for owner, rects in pairs(loose) do
        local clusters = {}
        for _, r in ipairs(rects) do
            table.insert(clusters, {x0 = r.x0, y0 = r.y0, x1 = r.x1, y1 = r.y1})
        end
        local merged = true
        while merged do
            merged = false
            for i = 1, #clusters do
                for j = #clusters, i + 1, -1 do
                    if rectGap(clusters[i], clusters[j]) <= VRHud.GAP then
                        unite(clusters[i], clusters[j])
                        table.remove(clusters, j)
                        merged = true
                    end
                end
            end
        end
        for _, cl in ipairs(clusters) do
            local host
            for _, d in ipairs(displayBlocks) do
                if rectGap(cl, d) <= VRHud.ATTACH then
                    host = d
                    break
                end
            end
            if host ~= nil then
                unite(host, cl)
            else
                cl.owner = owner
                table.insert(blocks, cl)
            end
        end
    end

    -- grow the known parts; a mod's block keeps its key while it stays near where it was
    local changed = false
    for _, b in ipairs(blocks) do
        if (b.x1 - b.x0) * (b.y1 - b.y0) <= VRHud.MAX_AREA then
            local key = b.key
            if key == nil then
                -- a part it overlaps, or a display's part it touches, takes it in
                for k, p in pairs(self.parts) do
                    local gap = rectGap(p, b)
                    if gap < 0 or string.sub(k, 1, 4) == "hud." and gap <= VRHud.ATTACH then
                        key = k
                        break
                    end
                end
            end
            if key == nil then
                local n = 0
                for k, p in pairs(self.parts) do
                    if p.owner == b.owner then
                        n = n + 1
                        if key == nil and rectGap(p, b) <= VRHud.GAP then
                            key = k
                        end
                    end
                end
                key = key or string.format("%s#%d", b.owner, n + 1)
            end
            local p = self.parts[key]
            if p == nil then
                self.parts[key] = {owner = b.owner, x0 = b.x0, y0 = b.y0, x1 = b.x1, y1 = b.y1}
                changed = true
            elseif b.x0 < p.x0 - 0.001 or b.y0 < p.y0 - 0.001 or b.x1 > p.x1 + 0.001 or b.y1 > p.y1 + 0.001 then
                unite(p, b)
                changed = true
            end
        end
    end
    if self:mergeOverlapping() then
        changed = true
    end
    if changed or not self.sent then
        self:send()
    end
end

-- overlapping parts would show the same pixels twice: the display's (or the larger) takes the other in
function VRHud:mergeOverlapping()
    local any = false
    local merged = true
    while merged do
        merged = false
        local keys = {}
        for k, _ in pairs(self.parts) do
            table.insert(keys, k)
        end
        table.sort(keys)
        for i = 1, #keys do
            for j = i + 1, #keys do
                local a, b = self.parts[keys[i]], self.parts[keys[j]]
                if rectGap(a, b) < -0.002 then
                    local keep, drop = keys[i], keys[j]
                    local aHud, bHud = string.sub(keep, 1, 4) == "hud.", string.sub(drop, 1, 4) == "hud."
                    local area = function(p) return (p.x1 - p.x0) * (p.y1 - p.y0) end
                    if bHud and not aHud or aHud == bHud and area(b) > area(a) then
                        keep, drop = drop, keep
                    end
                    unite(self.parts[keep], self.parts[drop])
                    self.parts[drop] = nil
                    for _, layout in pairs(self.layouts) do
                        if layout[keep] == nil then
                            layout[keep] = layout[drop]
                        end
                        layout[drop] = nil
                    end
                    merged, any = true, true
                    break
                end
            end
            if merged then
                break
            end
        end
    end
    return any
end

-- default place: the part where it is on the flat HUD, on a plane in front of the seat
function VRHud:defaultPlace(p)
    local _, distance, width, offsetY = self.api.hudPanel()
    local height = width * self:screenAspect()
    local cx, cy = 0.5 * (p.x0 + p.x1), 0.5 * (p.y0 + p.y1)
    return {(cx - 0.5) * width, (cy - 0.5) * height + offsetY, -distance, 0, 0, (p.x1 - p.x0) * width}
end

function VRHud:screenAspect()
    return (g_screenHeight or 9) / (g_screenWidth or 16)
end

function VRHud:send()
    local keys = {}
    for k, _ in pairs(self.parts) do
        table.insert(keys, k)
    end
    table.sort(keys)
    local layout = self.layouts[self.layoutId] or {}
    local n = math.min(#keys, VRHud.MAX_PANELS)
    local logIt = not self.editing
    local hoverGroup = self.hover ~= nil and self:membersOf(self.hover) or {}
    self.panels = {}
    local i = 0
    local function add(...)
        self.api.setHudPanel(i, ...)
        i = i + 1
    end
    for k = 1, n do
        local key = keys[k]
        local p = self.parts[key]
        local place = layout[key] or self:defaultPlace(p)
        local mark = 0
        if self.editing then
            if self.selected[key] then
                mark = self.holding and 3 or 4
            elseif hoverGroup[key] then
                mark = 2
            else
                mark = 1
            end
        end
        add(p.x0, 1 - p.y1, p.x1, 1 - p.y0, place[1], place[2], place[3], place[4], place[5], place[6], mark, 0)
        -- the bridge's height: the part's aspect on the screen
        self.panels[k] = {key = key, place = place,
            height = place[6] * (p.y1 - p.y0) / math.max(p.x1 - p.x0, 1e-6) * self:screenAspect()}
        if logIt then
            print(string.format("FS25_VR: HUD panel %d %s x %.3f..%.3f y %.3f..%.3f", k, key, p.x0, p.x1, p.y0, p.y1))
        end
    end
    if self.messageOn then
        -- the mod's messages, a little below the middle of the view
        local m = VRHud.MESSAGE
        add(m.x0, 1 - m.y1, m.x1, 1 - m.y0, 0, -0.1, -1, 0, -0.1, 0.6, 0, 1)
    end
    if self.editing then
        -- the help below the view and a crosshair in its middle, both moving with the head
        local h = VRHud.HELP
        if self.showHelp then
            add(h.x0, 1 - h.y1, h.x1, 1 - h.y0, 0, -0.22, -0.75, 0, -0.29, 0.36, 0, 1)
        end
        local a = self:screenAspect()
        local u1, v1 = math.min(1, a), math.min(1, 1 / a)  -- a square
        add(0, 0, u1, v1, 0, 0, -1, 0, 0, 0.012, 5, 3)
        add(0, 0, u1, v1, 0, 0, -0.999, 0, 0, 0.005, 1, 3)
    end
    if i == 0 then
        return
    end
    self.api.setHudPanelCount(i)
    self.sent = true
end

---------------------------------------------------------------------------------------------------
-- arranging

local function text(name, fallback)
    local env = g_i18n ~= nil and g_i18n.modEnvironments ~= nil and VRMod ~= nil and g_i18n.modEnvironments[VRMod.modName] or nil
    local ok, t = pcall(function() return (env or g_i18n):getText(name) end)
    return ok and t ~= nil and t ~= "" and not string.find(t, "Missing", 1, true) and t or fallback
end

function VRHud:setEditing(on)
    if on == self.editing then
        return
    end
    self.editing = on
    self.holding = false
    self.hover = nil
    self.selected = {}
    self.showHelp = true
    if self.api.setCursorVisible ~= nil then
        self.api.setCursorVisible(not on)
    end
    if g_inputBinding ~= nil and g_inputBinding.setShowMouseCursor ~= nil then
        if on then
            self.cursorWasShown = g_inputBinding:getShowMouseCursor()
            g_inputBinding:setShowMouseCursor(true)
        elseif not self.cursorWasShown then
            g_inputBinding:setShowMouseCursor(false)
        end
    end
    if not on then
        self:saveLayouts()
    end
    self:send()
end

function VRHud:isDown(left, right)
    local ok, down = pcall(function()
        return Input.isKeyPressed(left) or Input.isKeyPressed(right)
    end)
    return ok and down or false
end

function VRHud:setMessage(on)
    if on ~= self.messageOn then
        self.messageOn = on
        if self.sent or self.editing then
            self:send()
        end
    end
end

-- text at most maxWidth wide (screen units): the size, shrunk if needed
local function fitted(size, maxWidth, lines)
    if getTextWidth == nil then
        return size
    end
    local widest = 0
    for _, line in ipairs(lines) do
        widest = math.max(widest, getTextWidth(size, line))
    end
    return widest > maxWidth and size * maxWidth / widest or size
end

-- the mod's message and (arranging) the help, drawn into the HUD in their own places, where the
-- bridge picks them up as panels of their own (they are no part of the HUD's panels)
function VRHud:draw(message)
    self.drawingHelp = true
    if message ~= nil then
        local m = VRHud.MESSAGE
        setTextAlignment(RenderText.ALIGN_CENTER)
        setTextBold(true)
        renderText(0.5 * (m.x0 + m.x1), m.y0 + 0.012, fitted(0.025, m.x1 - m.x0 - 0.01, {message}), message)
        setTextBold(false)
        setTextAlignment(RenderText.ALIGN_LEFT)
    end
    if self.editing and self.showHelp then
        local h = VRHud.HELP
        local lines = {
            text("vr_hud_help_title", "Arrange the HUD"),
            text("vr_hud_help_aim", "Crosshair on a panel: it turns yellow"),
            text("vr_hud_help_move", "Hold the left mouse button: the panel follows your view"),
            text("vr_hud_help_select", "Shift+click: select several"),
            text("vr_hud_help_group", "G: group the selection / split the group"),
            text("vr_hud_help_wheel", "Wheel: distance; Shift+wheel: size"),
            text("vr_hud_help_turn", "Ctrl+wheel: tilt; Alt+wheel: turn"),
            text("vr_hud_help_reset", "Right click: back to the default place"),
            text("vr_hud_help_keys", "F10: hide this help; Shift+F10: done (saves)"),
        }
        drawFilledRect(h.x0, h.y0, h.x1 - h.x0, h.y1 - h.y0, 0, 0, 0, 0.75)
        setTextAlignment(RenderText.ALIGN_LEFT)
        setTextColor(1, 1, 1, 1)
        local step = (h.y1 - h.y0) / (#lines + 1)
        local size = fitted(0.62 * step, h.x1 - h.x0 - 0.02, lines)
        local y = h.y1 - 1.1 * step
        for k, line in ipairs(lines) do
            setTextBold(k == 1)
            renderText(h.x0 + 0.01, y, k == 1 and size * 1.2 or size, line)
            y = y - step
        end
        setTextBold(false)
        setTextColor(1, 1, 1, 1)
    end
    self.drawingHelp = false
end

-- the head's position and view direction in recentred tracking space
function VRHud:gaze()
    local ok, x, y, z, qx, qy, qz, qw = self.api.headPose()
    if not ok then
        return nil
    end
    return {x, y, z}, qrot({qx, qy, qz, qw}, {0, 0, -1})
end

-- the panel the view ray hits first
function VRHud:pick(origin, dir)
    local best, bestT
    for _, panel in ipairs(self.panels) do
        local pl = panel.place
        local q = panelRotation(pl[4], pl[5])
        local n, u, v = qrot(q, {0, 0, 1}), qrot(q, {1, 0, 0}), qrot(q, {0, 1, 0})
        local denom = dot(dir, n)
        if math.abs(denom) > 1e-6 then
            local c = {pl[1] - origin[1], pl[2] - origin[2], pl[3] - origin[3]}
            local t = dot(c, n) / denom
            if t > 0 and (bestT == nil or t < bestT) then
                local hit = {origin[1] + t * dir[1] - pl[1], origin[2] + t * dir[2] - pl[2], origin[3] + t * dir[3] - pl[3]}
                if math.abs(dot(hit, u)) <= 0.5 * pl[6] and math.abs(dot(hit, v)) <= 0.5 * panel.height then
                    best, bestT = panel, t
                end
            end
        end
    end
    return best
end

-- the layout entry of a part (made from its default place on first change)
function VRHud:placeOf(key)
    local layout = self.layouts[self.layoutId]
    if layout == nil then
        layout = {}
        self.layouts[self.layoutId] = layout
    end
    if layout[key] == nil and self.parts[key] ~= nil then
        layout[key] = self:defaultPlace(self.parts[key])
    end
    return layout[key]
end

-- a part and the parts grouped with it (key -> true)
function VRHud:membersOf(key)
    local set = {[key] = true}
    local layout = self.layouts[self.layoutId]
    local group = layout ~= nil and layout[key] ~= nil and layout[key][7] or nil
    if group ~= nil then
        for k, pl in pairs(layout) do
            if pl[7] == group and self.parts[k] ~= nil then
                set[k] = true
            end
        end
    end
    return set
end

-- what the wheel and the right button act on: the selection, else what is under the crosshair
function VRHud:targets()
    if next(self.selected) ~= nil then
        return self.selected
    end
    if self.hover ~= nil then
        return self:membersOf(self.hover)
    end
    return nil
end

local function yawPitch(d)
    return math.atan2(-d[1], -d[3]), math.asin(math.max(-1, math.min(1, d[2])))
end

function VRHud:updateEditing()
    local origin, dir = self:gaze()
    if origin == nil then
        return
    end
    local changed = false
    if self.holding then
        local count = 0
        for _ in pairs(self.selected) do
            count = count + 1
        end
        if count == 1 then
            -- one panel follows the view at its distance, facing the head
            local key = next(self.selected)
            local pl = self:placeOf(key)
            local d = math.max(self.grab.distance or 1, VRHud.MIN_DISTANCE)
            pl[1], pl[2], pl[3] = origin[1] + d * dir[1], origin[2] + d * dir[2], origin[3] + d * dir[3]
            self:face(pl, origin)
        else
            -- several move as one: turned about the head by the change of the view direction
            local g = self.grab
            local yaw0, pitch0 = yawPitch(g.dir)
            local yaw1, pitch1 = yawPitch(dir)
            local qBack = {0, math.sin(-0.5 * yaw0), 0, math.cos(-0.5 * yaw0)}
            local qPitch = {math.sin(0.5 * (pitch1 - pitch0)), 0, 0, math.cos(0.5 * (pitch1 - pitch0))}
            local qTo = {0, math.sin(0.5 * yaw1), 0, math.cos(0.5 * yaw1)}
            local q = qmul(qTo, qmul(qPitch, qBack))
            for key, start in pairs(g.places) do
                local pl = self:placeOf(key)
                local v = qrot(q, {start[1] - g.origin[1], start[2] - g.origin[2], start[3] - g.origin[3]})
                pl[1], pl[2], pl[3] = origin[1] + v[1], origin[2] + v[2], origin[3] + v[3]
                pl[4] = start[4] + (yaw1 - yaw0)
                pl[5] = start[5] + (pitch1 - pitch0)
            end
        end
        changed = true
    else
        local hit = self:pick(origin, dir)
        local key = hit ~= nil and hit.key or nil
        if key ~= self.hover then
            self.hover = key
            changed = true
        end
    end
    if changed then
        self:send()
    end
end

-- turns a panel to face the head
function VRHud:face(pl, origin)
    local h = {origin[1] - pl[1], origin[2] - pl[2], origin[3] - pl[3]}
    local len = math.sqrt(dot(h, h))
    if len > 1e-4 then
        pl[4] = math.atan2(h[1], h[3])
        pl[5] = -math.asin(math.max(-1, math.min(1, h[2] / len)))
    end
end

-- true if the key was used (only while arranging)
function VRHud:keyEvent(sym, isDown)
    if not self.editing or not isDown then
        return false
    end
    if sym == Input.KEY_g then
        self:toggleGroup()
        return true
    end
    return false
end

function VRHud:toggleShowHelp()
    self.showHelp = not self.showHelp
    self:send()
end

-- G: the selection becomes a group; a selection that is one group is split up again
function VRHud:toggleGroup()
    local keys = {}
    for k in pairs(self.selected) do
        table.insert(keys, k)
    end
    if #keys == 0 then
        return
    end
    local first = self:placeOf(keys[1])[7]
    local same = first ~= nil
    for _, k in ipairs(keys) do
        same = same and self:placeOf(k)[7] == first
    end
    local group = nil
    if not same then
        if #keys < 2 then
            return
        end
        group = 0
        for _, pl in pairs(self.layouts[self.layoutId] or {}) do
            group = math.max(group, pl[7] or 0)
        end
        group = group + 1
    end
    for _, k in ipairs(keys) do
        self:placeOf(k)[7] = group
    end
    if VRMod ~= nil and VRMod.showMessage ~= nil then
        VRMod:showMessage(group ~= nil and "HUD panels grouped" or "HUD panels ungrouped")
    end
    self:send()
end

function VRHud:mouseEvent(posX, posY, isDown, isUp, button)
    if not self.editing then
        return
    end
    local shift = self:isDown(Input.KEY_lshift, Input.KEY_rshift)
    if isDown and button == Input.MOUSE_BUTTON_LEFT then
        if self.hover == nil then
            if not shift then
                self.selected = {}
                self:send()
            end
            return
        end
        local members = self:membersOf(self.hover)
        if shift then
            -- add to / take out of the selection
            local inside = self.selected[self.hover] ~= nil
            for k in pairs(members) do
                self.selected[k] = (not inside) or nil
            end
            self:send()
            return
        end
        if self.selected[self.hover] == nil then
            self.selected = members
        end
        local origin, dir = self:gaze()
        if origin == nil then
            return
        end
        self.grab = {origin = origin, dir = dir, places = {}}
        for k in pairs(self.selected) do
            local pl = self:placeOf(k)
            self.grab.places[k] = {pl[1], pl[2], pl[3], pl[4], pl[5]}
            local d = {pl[1] - origin[1], pl[2] - origin[2], pl[3] - origin[3]}
            self.grab.distance = math.sqrt(dot(d, d))
        end
        self.holding = true
        self:send()
    elseif isUp and button == Input.MOUSE_BUTTON_LEFT and self.holding then
        self.holding = false
        if not shift then
            -- a single panel moved on its own is not kept selected
            local count = 0
            for _ in pairs(self.selected) do
                count = count + 1
            end
            if count == 1 then
                self.selected = {}
            end
        end
        self:send()
    elseif isDown and button == Input.MOUSE_BUTTON_RIGHT then
        local targets = self:targets()
        local layout = self.layouts[self.layoutId]
        if targets ~= nil and layout ~= nil then
            for k in pairs(targets) do
                if layout[k] ~= nil and self.parts[k] ~= nil then
                    local group = layout[k][7]
                    layout[k] = self:defaultPlace(self.parts[k])
                    layout[k][7] = group
                end
            end
        end
        self.holding = false
        self:send()
    elseif isDown and (button == Input.MOUSE_BUTTON_WHEEL_UP or button == Input.MOUSE_BUTTON_WHEEL_DOWN) then
        local targets = self:targets()
        local origin = self:gaze()
        if targets == nil or origin == nil then
            return
        end
        local up = button == Input.MOUSE_BUTTON_WHEEL_UP
        local f = up and VRHud.STEP or 1 / VRHud.STEP
        local step = up and VRHud.ANGLE_STEP or -VRHud.ANGLE_STEP
        local ctrl = self:isDown(Input.KEY_lctrl, Input.KEY_rctrl)
        local alt = self:isDown(Input.KEY_lalt, Input.KEY_ralt)
        -- size: about the targets' middle
        local mid = {0, 0, 0}
        local count = 0
        for k in pairs(targets) do
            local pl = self:placeOf(k)
            for c = 1, 3 do
                mid[c] = mid[c] + pl[c]
            end
            count = count + 1
        end
        for c = 1, 3 do
            mid[c] = mid[c] / count
        end
        for k in pairs(targets) do
            local pl = self:placeOf(k)
            if shift then
                pl[6] = math.max(0.02, pl[6] * f)
                for c = 1, 3 do
                    pl[c] = mid[c] + (pl[c] - mid[c]) * f
                end
            elseif ctrl then
                pl[5] = pl[5] + step
            elseif alt then
                pl[4] = pl[4] + step
            else
                -- distance: along the line from the head
                local d = {pl[1] - origin[1], pl[2] - origin[2], pl[3] - origin[3]}
                local len = math.sqrt(dot(d, d))
                if len > 1e-4 then
                    local newLen = math.max(VRHud.MIN_DISTANCE, len * f)
                    for c = 1, 3 do
                        pl[c] = origin[c] + d[c] * newLen / len
                    end
                end
            end
        end
        if self.holding and self.grab ~= nil then
            -- keep holding from where the panels are now
            local o, dir = self:gaze()
            self.grab.origin, self.grab.dir = o, dir
            for k in pairs(self.selected) do
                local pl = self:placeOf(k)
                self.grab.places[k] = {pl[1], pl[2], pl[3], pl[4], pl[5]}
                local d = {pl[1] - o[1], pl[2] - o[2], pl[3] - o[3]}
                self.grab.distance = math.sqrt(dot(d, d))
            end
        end
        self:send()
    end
end
