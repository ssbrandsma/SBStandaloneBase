local io, tonumber = io, tonumber
local Manager = {}
Manager.__index = Manager

function Manager:new(root) return setmetatable({root=root},self) end
local function parse(stream)
    local result={}
    for line in stream:lines() do
        local key,value=line:match("^([%w_]+)=(.*)$")
        if key then result[key]=tonumber(value) or value end
    end
    return result
end
function Manager:getStorageInfo()
    local p=io.popen(self.root.."/sb-storage-helper info 2>/dev/null","r")
    if not p then return {compatible="false",reason="helper_unavailable"} end
    local result=parse(p); p:close(); return result
end
function Manager:getCompatibility()
    local i=self:getStorageInfo()
    return {supported=i.compatible=="true",reason=i.reason or "unknown"}
end
function Manager:getExpansionOptions()
    local i=self:getStorageInfo()
    return {enabled=false,targetBytes=i.target_bytes,targetLebs=i.target_lebs}
end
function Manager:getExpansionStatus() return {enabled=false,state="validation_required"} end
return Manager
