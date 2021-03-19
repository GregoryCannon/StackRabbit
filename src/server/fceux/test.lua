local socket = require("socket")
local http = require("socket.http")

-- local r, c, h = http.request{ 
--     url = "http://localhost:3000/uu0,0,0,0,0,0,0,0,0,0v,u0,0,0,0,0,0,0,0,0,0v,u0,0,0,0,0,0,0,0,0,0v,u0,0,0,0,0,0,0,0,0,0v,u0,0,0,0,0,0,0,0,0,0v,u0,0,0,0,0,0,0,0,0,0v,u0,0,0,0,0,0,0,0,0,0v,u0,0,0,0,0,0,0,0,0,0v,u0,0,0,0,0,0,0,0,0,0v,u0,0,0,0,0,0,0,0,0,0v,u0,0,0,0,0,0,0,0,0,0v,u0,0,0,0,0,0,0,0,0,0v,u0,0,0,0,0,0,0,0,0,0v,u0,0,0,0,0,0,0,0,0,0v,u0,0,0,0,0,0,0,0,0,0v,u0,0,0,0,0,0,0,0,0,0v,u0,0,0,0,0,0,0,0,0,0v,u0,0,0,0,0,0,0,0,0,0v,u0,1,0,0,0,0,0,0,0,0v,u1,1,1,0,0,0,0,0,0,0vv/T/J/18/0", 
-- }

-- print(r)
-- print(c)
-- print(h)

local data = ""

local function collect(chunk)
  if chunk ~= nil then
    data = data .. chunk
  end
  return true
end

local ok, statusCode, headers, statusText = http.request {
  method = "GET",
  url = "http://localhost:3000/00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000001110/T/J/18/0",
  sink = collect
}

print(data)

-- print("I think it worked :D")
