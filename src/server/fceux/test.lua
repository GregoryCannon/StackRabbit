local os = require("os")

function testBackgroundProcess()
  -- Async
  result = io.popen("bgThread")
  
  -- Synchronous
  -- result = os.execute("bgThread")
  
  print(result)
  print("Done!")
end

-- testBackgroundProcess()

function testApiCall()
  local socket = require("socket")
  local http = require("socket.http")

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
end
