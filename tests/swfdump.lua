-- Copies every loaded Flash movie (SwfMovie.RawData) to <Game>\H7ModEngine\dumps\swf\ and logs the open movie
-- players. Copy into <Game>\H7ModEngine\exec\ at the main menu.
local function T(fmt, ...) h7.log("[swfdump] " .. fmt:format(...)) end

local exec_dir = debug.getinfo(1, "S").source:match("^@(.*)[/\\][^/\\]+$")
local out_dir = exec_dir .. "\\..\\dumps\\swf\\"
os.execute('mkdir "' .. out_dir .. '" 2>nul')

local MAX = 16 * 1024 * 1024
for _, movie in ipairs(h7.find_all("SwfMovie")) do
  local data = movie.RawData
  local n = #data
  local file = out_dir .. movie:get_path_name():gsub("[^%w%._-]", "_") .. ".swf"
  if n > 0 and n <= MAX then
    local f = assert(io.open(file, "wb"))
    local chunk = {}
    for i = 1, n do
      chunk[#chunk + 1] = data[i]
      if #chunk == 4096 or i == n then
        f:write(string.char(table.unpack(chunk)))
        chunk = {}
      end
    end
    f:close()
  end
  T("%-60s %9d bytes%s", movie:get_path_name(), n, n > MAX and " (skipped)" or "")
end

for _, p in ipairs(h7.find_all("GFxMoviePlayer")) do
  T("player %s movie=%s", tostring(p), tostring(p.MovieInfo))
end

local cntl = h7.first("H7MainMenuCntl")
T("main menu controller: %s", tostring(cntl))
if cntl then
  T("mMainMenu=%s mBtnOptions=%s mBtnQuit=%s", tostring(cntl.mMainMenu), tostring(cntl.mBtnOptions),
    tostring(cntl.mBtnQuit))
end
return "done"
