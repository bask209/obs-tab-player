-- SPDX-License-Identifier: Apache-2.0
-- OBS Tab Player: add this script in OBS (Tools > Scripts). It serves the overlay and the
-- control dock from a tiny built-in web server on 127.0.0.1, lists your tab library,
-- remembers the player state and adds global OBS hotkeys. No other programs are needed.

local obs = obslua
local ffi = require("ffi")
local bit = require("bit")

local VERSION = "0.1.0"
local DEFAULT_PORT = 8737
local MAX_BODY = 65536

local script_dir = script_path()
local www_dir = script_dir .. "www/"

local cfg = { port = DEFAULT_PORT, library = "" }
local state = nil          -- obs_data holding the player state
local status_json = "{}"   -- last status reported by the overlay
local library = {}         -- sorted list of { id, name, day, alt, size }
local library_json = "[]"
local settings_ref = nil
local hotkeys = {}

----------------------------------------------------------------------------- sockets (FFI)

local is_windows = ffi.os == "Windows"
local net = {}

local function cdef(s) pcall(ffi.cdef, s) end

if is_windows then
  cdef [[
    typedef uintptr_t SOCKET;
    typedef struct { unsigned short family; unsigned short port; unsigned int addr; char zero[8]; } tp_sockaddr_in;
    int WSAStartup(unsigned short ver, void *data);
    SOCKET socket(int af, int type, int protocol);
    int bind(SOCKET s, const void *addr, int len);
    int listen(SOCKET s, int backlog);
    SOCKET accept(SOCKET s, void *addr, int *len);
    int recv(SOCKET s, char *buf, int len, int flags);
    int send(SOCKET s, const char *buf, int len, int flags);
    int closesocket(SOCKET s);
    int ioctlsocket(SOCKET s, long cmd, unsigned long *arg);
    int setsockopt(SOCKET s, int level, int name, const char *val, int len);
    int WSAGetLastError(void);
    unsigned short htons(unsigned short v);
    int MultiByteToWideChar(unsigned int cp, unsigned long flags, const char *str, int n, wchar_t *out, int outn);
    void *_wfopen(const wchar_t *name, const wchar_t *mode);
    size_t fread(void *buf, size_t size, size_t n, void *f);
    int fclose(void *f);
  ]]
  local ws = ffi.load("ws2_32")
  local data = ffi.new("char[512]")
  ws.WSAStartup(0x0202, data)
  local INVALID = ffi.cast("SOCKET", -1)
  net.invalid = function(s) return s == INVALID end
  net.socket = function() return ws.socket(2, 1, 6) end
  -- FIONBIO is 0x8004667E, passed as a signed 32-bit long.
  net.nonblock = function(s) local one = ffi.new("unsigned long[1]", 1); return ws.ioctlsocket(s, -2147195266, one) == 0 end
  net.nodelay = function(s) local one = ffi.new("int[1]", 1); ws.setsockopt(s, 6, 1, ffi.cast("const char*", one), 4) end
  net.exclusive = function(s) local one = ffi.new("int[1]", 1); ws.setsockopt(s, 0xffff, -5, ffi.cast("const char*", one), 4) end
  net.bind = function(s, port)
    local a = ffi.new("tp_sockaddr_in")
    a.family = 2; a.port = ws.htons(port); a.addr = 0x0100007F -- 127.0.0.1
    return ws.bind(s, a, ffi.sizeof(a)) == 0
  end
  net.listen = function(s) return ws.listen(s, 16) == 0 end
  net.accept = function(s) return ws.accept(s, nil, nil) end
  net.recv = function(s, buf, n) return ws.recv(s, buf, n, 0) end
  net.send = function(s, str, off, n) return ws.send(s, ffi.cast("const char*", str) + off, n, 0) end
  net.close = function(s) ws.closesocket(s) end
  net.would_block = function() return ws.WSAGetLastError() == 10035 end

  -- UTF-8 safe binary file read (io.open uses the ANSI code page on Windows)
  local function wide(s)
    local n = ffi.C.MultiByteToWideChar(65001, 0, s, -1, nil, 0)
    local w = ffi.new("wchar_t[?]", n)
    ffi.C.MultiByteToWideChar(65001, 0, s, -1, w, n)
    return w
  end
  local rb = wide("rb")
  net.read_file = function(path)
    local f = ffi.C._wfopen(wide(path), rb)
    if f == nil then return nil end
    local parts, buf = {}, ffi.new("char[65536]")
    while true do
      local n = tonumber(ffi.C.fread(buf, 1, 65536, f))
      if n <= 0 then break end
      parts[#parts + 1] = ffi.string(buf, n)
    end
    ffi.C.fclose(f)
    return table.concat(parts)
  end
else
  local mac = ffi.os == "OSX"
  if mac then
    cdef [[ typedef struct { uint8_t len; uint8_t family; unsigned short port; unsigned int addr; char zero[8]; } tp_sockaddr_in; ]]
  else
    cdef [[ typedef struct { unsigned short family; unsigned short port; unsigned int addr; char zero[8]; } tp_sockaddr_in; ]]
  end
  cdef [[
    int socket(int domain, int type, int protocol);
    int bind(int s, const void *addr, unsigned int len);
    int listen(int s, int backlog);
    int accept(int s, void *addr, void *len);
    long recv(int s, char *buf, size_t len, int flags);
    long send(int s, const char *buf, size_t len, int flags);
    int close(int fd);
    int fcntl(int fd, int cmd, ...);
    int setsockopt(int s, int level, int name, const void *val, unsigned int len);
  ]]
  local C = ffi.C
  local NONBLOCK = mac and 0x0004 or 0x800
  local EAGAIN = mac and 35 or 11
  local NOSIGNAL = mac and 0 or 0x4000
  net.invalid = function(s) return s < 0 end
  net.socket = function() return C.socket(2, 1, 6) end
  net.nonblock = function(s) return C.fcntl(s, 4, ffi.new("int", bit.bor(C.fcntl(s, 3, ffi.new("int", 0)), NONBLOCK))) == 0 end
  net.nodelay = function(s) local one = ffi.new("int[1]", 1); C.setsockopt(s, 6, 1, one, 4) end
  net.exclusive = function(s)
    if mac then local one = ffi.new("int[1]", 1); C.setsockopt(s, 0xffff, 0x1022, one, 4) end -- SO_NOSIGPIPE
  end
  net.bind = function(s, port)
    local a = ffi.new("tp_sockaddr_in")
    if mac then a.len = ffi.sizeof(a) end
    a.family = 2; a.port = bit.bor(bit.rshift(port, 8), bit.lshift(bit.band(port, 0xff), 8)); a.addr = 0x0100007F
    return C.bind(s, a, ffi.sizeof(a)) == 0
  end
  net.listen = function(s) return C.listen(s, 16) == 0 end
  net.accept = function(s)
    local c = C.accept(s, nil, nil)
    if c >= 0 and mac then local one = ffi.new("int[1]", 1); C.setsockopt(c, 0xffff, 0x1022, one, 4) end
    return c
  end
  net.recv = function(s, buf, n) return tonumber(C.recv(s, buf, n, 0)) end
  net.send = function(s, str, off, n) return tonumber(C.send(s, ffi.cast("const char*", str) + off, n, NOSIGNAL)) end
  net.close = function(s) C.close(s) end
  net.would_block = function() return ffi.errno() == EAGAIN end
  net.read_file = function(path)
    local f = io.open(path, "rb")
    if not f then return nil end
    local d = f:read("*a")
    f:close()
    return d
  end
end

----------------------------------------------------------------------------- helpers

local function json_str(s)
  return '"' .. s:gsub('[%c"\\]', function(c)
    local map = { ['"'] = '\\"', ['\\'] = '\\\\', ['\n'] = '\\n', ['\r'] = '\\r', ['\t'] = '\\t' }
    return map[c] or string.format("\\u%04x", c:byte())
  end) .. '"'
end

local function url_decode(s)
  s = s:gsub("+", " ")
  return (s:gsub("%%(%x%x)", function(h) return string.char(tonumber(h, 16)) end))
end

local function query_param(q, key)
  for k, v in (q or ""):gmatch("([^&=]+)=([^&]*)") do
    if url_decode(k) == key then return url_decode(v) end
  end
  return nil
end

local MIME = {
  html = "text/html; charset=utf-8", js = "text/javascript; charset=utf-8", css = "text/css; charset=utf-8",
  json = "application/json", svg = "image/svg+xml", png = "image/png", ico = "image/x-icon",
  woff2 = "font/woff2",
}

----------------------------------------------------------------------------- library

local function parse_name(rel)
  local base = rel:match("([^/]+)$") or rel
  local stem, ext = base:match("^(.*)%.([%w]+)$")
  stem = stem or base
  local day, alt, topic = stem:match("^[Dd]ay%s*(%d+)%s*%-%s*(%u)%s*%-%s*(.+)$")
  if not day then day, topic = stem:match("^[Dd]ay%s*(%d+)%s*%-%s*(.+)$") end
  return { name = stem, day = day and tonumber(day) or nil, alt = alt or "", ext = (ext or ""):lower() }
end

local GP_EXT = { gp = true, gp3 = true, gp4 = true, gp5 = true, gpx = true, gp7 = true, gp8 = true }

local function scan_dir(root, rel, out, depth)
  if depth > 6 then return end
  local dir = obs.os_opendir(root .. (rel ~= "" and ("/" .. rel) or ""))
  if not dir then return end
  local entry = obs.os_readdir(dir)
  local subdirs = {}
  while entry do
    local name = entry.d_name
    if name:sub(1, 1) ~= "." and name:sub(1, 1) ~= "_" then
      local path = rel ~= "" and (rel .. "/" .. name) or name
      if entry.directory then
        subdirs[#subdirs + 1] = path
      else
        local ext = (name:match("%.([%w]+)$") or ""):lower()
        if GP_EXT[ext] then out[#out + 1] = path end
      end
    end
    entry = obs.os_readdir(dir)
  end
  obs.os_closedir(dir)
  for _, d in ipairs(subdirs) do scan_dir(root, d, out, depth + 1) end
end

local function scan_library()
  local files = {}
  if cfg.library ~= "" then scan_dir(cfg.library, "", files, 0) end
  library = {}
  for _, rel in ipairs(files) do
    local item = parse_name(rel)
    item.id = rel
    library[#library + 1] = item
  end
  table.sort(library, function(a, b)
    if (a.day == nil) ~= (b.day == nil) then return a.day ~= nil end
    if a.day ~= b.day then return a.day < b.day end
    if a.alt ~= b.alt then return a.alt < b.alt end
    return a.id < b.id
  end)
  local parts = {}
  for i, f in ipairs(library) do parts[i] = "{\"id\":" .. json_str(f.id) .. "}" end
  library_json = "[" .. table.concat(parts, ",") .. "]"
  if state and #library > 0 then
    local cur = obs.obs_data_get_string(state, "fileId")
    local found = false
    for _, f in ipairs(library) do if f.id == cur then found = true break end end
    if not found then obs.obs_data_set_string(state, "fileId", library[1].id) end
  end
end

local function find_library_index(id)
  for i, f in ipairs(library) do if f.id == id then return i end end
  return nil
end

----------------------------------------------------------------------------- state + events

local server = nil
local clients = {}   -- open connections
local streams = {}   -- SSE connections

local function send_raw(c, data)
  c.out[#c.out + 1] = data
end

local function broadcast(event, data)
  local msg = "event: " .. event .. "\ndata: " .. data .. "\n\n"
  for c in pairs(streams) do send_raw(c, msg) end
end

local function state_json()
  return obs.obs_data_get_json(state)
end

local function set_file(id)
  obs.obs_data_set_string(state, "fileId", id)
  obs.obs_data_set_int(state, "loopFrom", 0)
  obs.obs_data_set_int(state, "loopTo", 0)
end

local function step_file(dir)
  if #library == 0 then return end
  local i = find_library_index(obs.obs_data_get_string(state, "fileId")) or 1
  i = ((i - 1 + dir) % #library) + 1
  set_file(library[i].id)
end

local function step_day(dir)
  local cur = library[find_library_index(obs.obs_data_get_string(state, "fileId")) or 1]
  local cur_day = cur and cur.day or 0
  local mains = {}
  for _, f in ipairs(library) do if f.day and f.alt == "" then mains[#mains + 1] = f end end
  if #mains == 0 then return step_file(dir) end
  local target = nil
  if dir > 0 then
    for _, f in ipairs(mains) do if f.day > cur_day then target = f break end end
    target = target or mains[1]
  else
    for i = #mains, 1, -1 do if mains[i].day < cur_day then target = mains[i] break end end
    target = target or mains[#mains]
  end
  set_file(target.id)
end

local TRANSPORT = { play = true, pause = true, playPause = true, stop = true, restart = true, seek = true }

local function handle_command(cmd_type, values, step)
  if TRANSPORT[cmd_type] then
    broadcast("transport", "{\"type\":" .. json_str(cmd_type) .. ",\"value\":" .. string.format("%.3f", tonumber(step) or 0) .. "}")
    return
  end
  local round = function(v) return math.floor(v * 100 + 0.5) / 100 end
  if cmd_type == "set" and values then
    obs.obs_data_apply(state, values)
  elseif cmd_type == "speedUp" or cmd_type == "speedDown" then
    local s = obs.obs_data_get_double(state, "speed")
    local d = (step and step > 0) and step or 0.05
    s = cmd_type == "speedUp" and s + d or s - d
    obs.obs_data_set_double(state, "speed", round(math.max(0.1, math.min(3, s))))
  elseif cmd_type == "speedReset" then
    obs.obs_data_set_double(state, "speed", 1)
  elseif cmd_type == "nextFile" then step_file(1)
  elseif cmd_type == "prevFile" then step_file(-1)
  elseif cmd_type == "nextDay" then step_day(1)
  elseif cmd_type == "prevDay" then step_day(-1)
  elseif cmd_type == "toggleLoop" or cmd_type == "toggleCountIn" or cmd_type == "toggleMetronome" or cmd_type == "toggleNotes" then
    local key = ({ toggleLoop = "loop", toggleCountIn = "countIn", toggleMetronome = "metronome", toggleNotes = "showNotes" })[cmd_type]
    obs.obs_data_set_bool(state, key, not obs.obs_data_get_bool(state, key))
  else
    return
  end
  broadcast("state", state_json())
end

----------------------------------------------------------------------------- HTTP

local function respond(c, code, ctype, body, extra)
  local reason = ({ [200] = "OK", [204] = "No Content", [302] = "Found", [400] = "Bad Request", [403] = "Forbidden", [404] = "Not Found", [413] = "Too Large" })[code] or "OK"
  local head = "HTTP/1.1 " .. code .. " " .. reason .. "\r\nContent-Type: " .. ctype ..
    "\r\nContent-Length: " .. #body .. "\r\nCache-Control: no-cache\r\nConnection: close\r\n" .. (extra or "") .. "\r\n"
  send_raw(c, head)
  send_raw(c, body)
  c.close_after = true
end

local static_cache = {}

local function serve_static(c, path)
  if path:find("%.%.") or path:find("\\") then return respond(c, 403, "text/plain", "Forbidden") end
  local data = static_cache[path]
  if not data then
    data = net.read_file(www_dir .. path)
    if not data then return respond(c, 404, "text/plain", "Not found") end
    static_cache[path] = data
  end
  local ext = (path:match("%.([%w]+)$") or ""):lower()
  respond(c, 200, MIME[ext] or "application/octet-stream", data)
end

local function handle_request(c, method, target, body)
  local path, query = target:match("^([^?]*)%??(.*)$")
  path = url_decode(path or "/")
  if path == "/" then return respond(c, 302, "text/plain", "", "Location: /dock.html\r\n") end
  if path == "/events" then
    send_raw(c, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-store\r\nConnection: keep-alive\r\n\r\n")
    send_raw(c, "event: state\ndata: " .. state_json() .. "\n\n")
    send_raw(c, "event: status\ndata: " .. status_json .. "\n\n")
    c.role = query_param(query, "role") or "dock"
    streams[c] = true
    return
  end
  if path == "/api/library" then
    if query_param(query, "rescan") then scan_library(); broadcast("library", "{}") end
    return respond(c, 200, "application/json", "{\"root\":" .. json_str(cfg.library) .. ",\"version\":" .. json_str(VERSION) .. ",\"files\":" .. library_json .. "}")
  end
  if path == "/api/state" then return respond(c, 200, "application/json", state_json()) end
  if path == "/api/file" then
    local id = query_param(query, "id") or ""
    if not find_library_index(id) or id:find("%.%.") then return respond(c, 404, "text/plain", "Unknown file") end
    local data = net.read_file(cfg.library .. "/" .. id)
    if not data then return respond(c, 404, "text/plain", "Cannot read file") end
    return respond(c, 200, "application/octet-stream", data)
  end
  if path == "/api/status" and method == "POST" then
    status_json = body ~= "" and body or "{}"
    broadcast("status", status_json)
    return respond(c, 204, "text/plain", "")
  end
  if path == "/api/command" and method == "POST" then
    local d = obs.obs_data_create_from_json(body)
    if not d then return respond(c, 400, "text/plain", "Bad JSON") end
    local values = obs.obs_data_get_obj(d, "values")
    handle_command(obs.obs_data_get_string(d, "type"), values, obs.obs_data_get_double(d, "step"))
    if values then obs.obs_data_release(values) end
    obs.obs_data_release(d)
    return respond(c, 200, "application/json", "{\"ok\":true}")
  end
  local cmd = path:match("^/api/command/(%w+)$")
  if cmd then
    handle_command(cmd, nil, tonumber(query_param(query, "step") or ""))
    return respond(c, 200, "application/json", "{\"ok\":true}")
  end
  if method ~= "GET" then return respond(c, 400, "text/plain", "Bad request") end
  return serve_static(c, path:sub(2))
end

local recv_buf = ffi.new("char[16384]")

local function close_client(c)
  net.close(c.sock)
  clients[c] = nil
  streams[c] = nil
end

local function poll_client(c)
  -- read
  if not c.handled then
    while true do
      local n = net.recv(c.sock, recv_buf, 16384)
      if n > 0 then
        c.inb = c.inb .. ffi.string(recv_buf, n)
        if #c.inb > MAX_BODY + 8192 then respond(c, 413, "text/plain", "Too large"); c.handled = true; break end
      elseif n == 0 then
        close_client(c); return
      else
        if not net.would_block() then close_client(c); return end
        break
      end
    end
    if not c.handled then
      local head_end = c.inb:find("\r\n\r\n", 1, true)
      if head_end then
        local head = c.inb:sub(1, head_end - 1)
        local method, target = head:match("^(%u+) (%S+) HTTP")
        local host = (head:lower():match("\r\nhost:%s*([^\r]+)") or ""):gsub("%s+$", "")
        local allowed = host == "127.0.0.1:" .. cfg.port or host == "localhost:" .. cfg.port
        local len = tonumber(head:lower():match("\r\ncontent%-length:%s*(%d+)") or "0")
        local body = c.inb:sub(head_end + 4)
        if not method then respond(c, 400, "text/plain", "Bad request"); c.handled = true
        elseif not allowed then respond(c, 403, "text/plain", "Forbidden host"); c.handled = true
        elseif #body >= len then
          c.handled = true
          handle_request(c, method, target, body:sub(1, len))
        end
      end
    end
  elseif streams[c] then
    -- detect closed event streams
    local n = net.recv(c.sock, recv_buf, 16384)
    if n == 0 or (n < 0 and not net.would_block()) then close_client(c); return end
  end
  -- write
  while #c.out > 0 do
    local chunk = c.out[1]
    local n = net.send(c.sock, chunk, c.off, #chunk - c.off)
    if n > 0 then
      c.off = c.off + n
      if c.off >= #chunk then table.remove(c.out, 1); c.off = 0 end
    elseif n < 0 and not net.would_block() then
      close_client(c); return
    else
      break
    end
  end
  if c.close_after and #c.out == 0 then close_client(c) end
end

local last_ping = 0

local function poll()
  if not server then return end
  for _ = 1, 32 do
    local s = net.accept(server)
    if net.invalid(s) then break end
    if net.nonblock(s) then
      net.nodelay(s)
      clients[{ sock = s, inb = "", out = {}, off = 0 }] = true
    else
      net.close(s)
    end
  end
  for c in pairs(clients) do poll_client(c) end
  local now = os.time()
  if now - last_ping >= 15 then
    last_ping = now
    for c in pairs(streams) do send_raw(c, ": ping\n\n") end
  end
end

local function stop_server()
  obs.timer_remove(poll)
  for c in pairs(clients) do net.close(c.sock) end
  clients, streams = {}, {}
  if server then net.close(server) end
  server = nil
end

local function start_server()
  stop_server()
  local s = net.socket()
  if net.invalid(s) then obs.script_log(obs.LOG_WARNING, "Tab Player: cannot create socket"); return end
  net.exclusive(s)
  if not net.bind(s, cfg.port) or not net.listen(s) then
    net.close(s)
    obs.script_log(obs.LOG_WARNING, "Tab Player: port " .. cfg.port .. " is in use. Pick another port in the script settings.")
    return
  end
  -- Never run a blocking socket on OBS's render thread.
  if not net.nonblock(s) then
    net.close(s)
    obs.script_log(obs.LOG_WARNING, "Tab Player: cannot make the server socket non-blocking")
    return
  end
  server = s
  obs.timer_add(poll, 15)
  obs.script_log(obs.LOG_INFO, "Tab Player " .. VERSION .. " serving http://127.0.0.1:" .. cfg.port .. "/")
end

----------------------------------------------------------------------------- OBS integration

local function base_url() return "http://127.0.0.1:" .. cfg.port end

local function add_overlay_to_scene()
  local scene_source = obs.obs_frontend_get_current_scene()
  if not scene_source then return false end
  local scene = obs.obs_scene_from_source(scene_source)
  local s = obs.obs_data_create()
  obs.obs_data_set_string(s, "url", base_url() .. "/overlay.html")
  obs.obs_data_set_int(s, "width", 960)
  obs.obs_data_set_int(s, "height", 540)
  obs.obs_data_set_bool(s, "reroute_audio", true)
  obs.obs_data_set_bool(s, "shutdown", false)
  obs.obs_data_set_bool(s, "restart_when_active", false)
  obs.obs_data_set_string(s, "css", "")
  local name = "Tab Player"
  local existing = obs.obs_get_source_by_name(name)
  if existing then obs.obs_source_release(existing); name = name .. " " .. os.time() end
  local src = obs.obs_source_create("browser_source", name, s, nil)
  obs.obs_scene_add(scene, src)
  obs.obs_source_release(src)
  obs.obs_data_release(s)
  obs.obs_source_release(scene_source)
  return true
end

local HOTKEYS = {
  { "playPause", "Tab Player: Play / Pause" },
  { "stop", "Tab Player: Stop" },
  { "restart", "Tab Player: Restart" },
  { "nextDay", "Tab Player: Next day" },
  { "prevDay", "Tab Player: Previous day" },
  { "nextFile", "Tab Player: Next tab / version" },
  { "prevFile", "Tab Player: Previous tab / version" },
  { "speedUp", "Tab Player: Faster (+5%)" },
  { "speedDown", "Tab Player: Slower (-5%)" },
  { "speedReset", "Tab Player: Speed 1x" },
  { "toggleLoop", "Tab Player: Loop on/off" },
  { "toggleMetronome", "Tab Player: Metronome on/off" },
  { "toggleCountIn", "Tab Player: Count-in on/off" },
  { "toggleNotes", "Tab Player: Exercise text on/off" },
}

function script_description()
  return [[<h3>OBS Tab Player ]] .. VERSION .. [[</h3>
<p>Scrolling Guitar Pro tabs with synced audio, inside OBS.</p>
<ol>
<li>Pick your <b>tab folder</b> below.</li>
<li>Click <b>Add overlay to current scene</b>.</li>
<li>Add the controls: <b>Docks &gt; Custom Browser Docks</b>, name <i>Tab Player</i>, URL <b>]] .. base_url() .. [[/dock.html</b></li>
</ol>
<p>Hotkeys: <b>Settings &gt; Hotkeys</b>, search "Tab Player".</p>]]
end

function script_properties()
  local p = obs.obs_properties_create()
  obs.obs_properties_add_path(p, "library", "Tab folder", obs.OBS_PATH_DIRECTORY, "", nil)
  obs.obs_properties_add_int(p, "port", "Port", 1024, 65535, 1)
  obs.obs_properties_add_button(p, "add_overlay", "Add overlay to current scene", function()
    add_overlay_to_scene()
    return false
  end)
  obs.obs_properties_add_button(p, "rescan", "Rescan tab folder", function()
    scan_library()
    broadcast("library", "{}")
    return false
  end)
  local info = obs.obs_properties_add_text(p, "dock_url", "Dock URL", obs.OBS_TEXT_INFO)
  obs.obs_property_set_long_description(info, base_url() .. "/dock.html")
  return p
end

function script_defaults(settings)
  obs.obs_data_set_default_int(settings, "port", DEFAULT_PORT)
  obs.obs_data_set_default_string(settings, "dock_url", "http://127.0.0.1:" .. DEFAULT_PORT .. "/dock.html")
end

function script_update(settings)
  settings_ref = settings
  local port = obs.obs_data_get_int(settings, "port")
  local library_path = obs.obs_data_get_string(settings, "library"):gsub("\\", "/"):gsub("/$", "")
  obs.obs_data_set_string(settings, "dock_url", "http://127.0.0.1:" .. port .. "/dock.html")
  local restart = port ~= cfg.port or not server
  local rescan = library_path ~= cfg.library
  cfg.port = port
  cfg.library = library_path
  if rescan then
    scan_library()
    broadcast("library", "{}")
    broadcast("state", state_json())
  end
  if restart then start_server() end
end

local function load_state(settings)
  local saved = obs.obs_data_get_string(settings, "player_state")
  state = (saved ~= "" and obs.obs_data_create_from_json(saved)) or obs.obs_data_create()
  obs.obs_data_set_default_double(state, "speed", 1)
  obs.obs_data_set_default_bool(state, "loop", true)
  obs.obs_data_set_default_bool(state, "countIn", true)
  obs.obs_data_set_default_bool(state, "metronome", false)
  obs.obs_data_set_default_bool(state, "showNotes", true)
end

function script_load(settings)
  load_state(settings)
  cfg.port = obs.obs_data_get_int(settings, "port")
  cfg.library = obs.obs_data_get_string(settings, "library"):gsub("\\", "/"):gsub("/$", "")
  scan_library()
  for _, h in ipairs(HOTKEYS) do
    local cmd = h[1]
    local id = obs.obs_hotkey_register_frontend("tab_player_" .. cmd, h[2], function(pressed)
      if pressed then handle_command(cmd) end
    end)
    local arr = obs.obs_data_get_array(settings, "hotkey_" .. cmd)
    obs.obs_hotkey_load(id, arr)
    obs.obs_data_array_release(arr)
    hotkeys[cmd] = id
  end
  start_server()
end

function script_save(settings)
  if state then obs.obs_data_set_string(settings, "player_state", state_json()) end
  for cmd, id in pairs(hotkeys) do
    local arr = obs.obs_hotkey_save(id)
    obs.obs_data_set_array(settings, "hotkey_" .. cmd, arr)
    obs.obs_data_array_release(arr)
  end
end

function script_unload()
  stop_server()
  if state then obs.obs_data_release(state) end
  state = nil
end
