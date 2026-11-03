-- Hook behaviour test: copy into <Game>\H7ModEngine\exec\ at the main menu; results go to logs\engine.log.
local function T(name, v) h7.log(("[hooktest] %-18s %s"):format(name, tostring(v))) end
local pc = h7.first("PlayerController")
-- most-derived GetHumanReadableName along pc's class chain
local fnPath
local cls = pc:get_class()
while cls and not fnPath do
  local p = cls:get_path_name() .. ".GetHumanReadableName"
  if h7.find("Function " .. p) then fnPath = p end
  cls = cls:get_super()
end
T("target", fnPath)
T("original", pc:GetHumanReadableName())

-- 1) pre-hook that blocks
local id = h7.hook(fnPath, function(self, args) return true end)
T("blocked", "'" .. tostring(pc:GetHumanReadableName()) .. "'")
h7.unhook(id)
-- 2) post-hook overriding the return value
id = h7.hook_post(fnPath, function(self, args, ret) return "Modded " .. tostring(ret) end)
T("overridden", pc:GetHumanReadableName())
h7.unhook(id)
T("restored", pc:GetHumanReadableName())

-- 3) reading params from a hooked event (FFrame locals)
local seen
local hid = h7.hook("Engine.GameViewportClient.PostRender", function(self, args)
  if not seen then seen = tostring(args and args.Canvas) end
end)
h7.after(0.5, function() T("PostRender.Canvas", seen); h7.unhook(hid) end)

-- 4) script-to-script calls: PostRender's bytecode calls DrawTransition(Canvas) every frame
local n, canvas = 0, nil
local did = h7.hook("Engine.GameViewportClient.DrawTransition", function(self, args)
  n = n + 1
  canvas = canvas or tostring(args.Canvas)
end)
h7.after(1, function() T("script->script", n .. " calls, Canvas=" .. tostring(canvas)); h7.unhook(did) end)
T("signature", h7.signature(fnPath))
return "ok3"
