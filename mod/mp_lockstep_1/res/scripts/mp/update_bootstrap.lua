-- Stable installed entry point. The proxy pins this path once per process.
local M = {}
local installed = false
function M.setup()
    if installed then return end
    local root = os.getenv("TPF2MP_RELEASE_ROOT")
    if root and root ~= "" then
        local searchers = package.searchers or package.loaders
        table.insert(searchers, 1, function(name)
            if not name:match("^mp[/.]") then return nil end
            local leaf = name:sub(4):gsub("%.", "/")
            assert(leaf:match("^[%w_/]+$"), "invalid multiplayer module")
            local path = root .. "/mod/res/scripts/mp/" .. leaf .. ".lua"
            local chunk, err = loadfile(path)
            -- Do not fall through to old installed modules on a damaged update.
            assert(chunk, "Multiplayer update is incomplete: " .. tostring(err))
            return chunk
        end)
    end
    installed = true
end
return M
