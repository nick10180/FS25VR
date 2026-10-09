-- F11: VR settings (stereo mode, 3D resolution). Applied with OK, saved by VRMod.

VRSettingsDialog = {}
local VRSettingsDialog_mt = Class(VRSettingsDialog, DialogElement)

VRSettingsDialog.MODE_ALTERNATING = 1
VRSettingsDialog.MODE_PLANE = 2
VRSettingsDialog.MODE_QUAD = 3

-- the game's 3D resolution steps (its graphics menu offers the same)
VRSettingsDialog.SCALES = {}
for i = 5, 20 do
    table.insert(VRSettingsDialog.SCALES, i / 10)
end

function VRSettingsDialog.new(target, custom_mt)
    local self = DialogElement.new(target, custom_mt or VRSettingsDialog_mt)
    self.mod = nil
    return self
end

local function text(name)
    local env = g_i18n.modEnvironments ~= nil and g_i18n.modEnvironments[VRMod.modName] or nil
    return (env or g_i18n):getText(name)
end

local function scaleIndex(scale)
    local best, bestDiff = 6, math.huge
    for i, s in ipairs(VRSettingsDialog.SCALES) do
        local d = math.abs(s - scale)
        if d < bestDiff then
            best, bestDiff = i, d
        end
    end
    return best
end

function VRSettingsDialog:onOpen()
    VRSettingsDialog:superClass().onOpen(self)
    local mod = self.mod
    self.guiTitle:setText(text("vr_settings_title"))
    self.stereoModeTitle:setText(text("vr_settings_stereoMode"))
    self.stereoModeSetting:setTexts({text("vr_settings_modeAlternating"), text("vr_settings_modePlane"),
        text("vr_settings_modeQuad")})
    self.stereoModeSetting:setState(mod:getStereoMode())
    self.renderScaleTitle:setText(text("vr_settings_renderScale"))
    local texts = {}
    for _, s in ipairs(VRSettingsDialog.SCALES) do
        table.insert(texts, string.format("%d%%", math.floor(s * 100 + 0.5)))
    end
    self.renderScaleSetting:setTexts(texts)
    self.renderScaleSetting:setState(scaleIndex(mod:getRenderScale()))
    self:updateInfo()
end

-- what reaches the headset: pixel density of the eye's image and of the focus view, as a multiple
-- of the headset's recommended resolution
function VRSettingsDialog:updateInfo()
    local mod = self.mod
    local scale = VRSettingsDialog.SCALES[self.renderScaleSetting:getState()] or 1
    local recW, _, focusW = 0, 0, 0.5
    if mod.api ~= nil and mod.api.headsetInfo ~= nil then
        recW, _, focusW = mod.api.headsetInfo()
    end
    if recW == nil or recW == 0 or g_screenWidth == nil then
        self.densityInfo:setText("")
        return
    end
    local w, h = math.floor(g_screenWidth * scale + 0.5), math.floor(g_screenHeight * scale + 0.5)
    local eye = w / recW
    local info
    if self.stereoModeSetting:getState() == VRSettingsDialog.MODE_QUAD then
        info = string.format(text("vr_settings_infoQuad"), w, h, eye / math.max(focusW, 0.1), eye)
    else
        info = string.format(text("vr_settings_info"), w, h, eye)
    end
    self.densityInfo:setText(info)
end

function VRSettingsDialog:onClickStereoMode()
    self:updateInfo()
end

function VRSettingsDialog:onClickRenderScale()
    self:updateInfo()
end

function VRSettingsDialog:onClickOk()
    local mode = self.stereoModeSetting:getState()
    local scale = VRSettingsDialog.SCALES[self.renderScaleSetting:getState()] or 1
    g_gui:closeDialogByName("VRSettingsDialog")
    self.mod:applySettings(mode, scale)
end

function VRSettingsDialog:onClickBack()
    g_gui:closeDialogByName("VRSettingsDialog")
end
