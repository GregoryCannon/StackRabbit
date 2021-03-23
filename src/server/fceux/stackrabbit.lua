local http = require("socket.http")
require "socket"

-- Convenience functions to translate internal Piece IDs to actual piece types
--T: 2 J: 7 Z: 9 O: 10 S: 11 L: 15 I: 18
orientToPiece = {[0]="none", [2]="T", [7]="J", [8]="Z", [10]="O", [11]="S", [14]="L", [18]="I"}
orientToNum = {[0]="none", [2]=1, [7]=2, [8]=3, [10]=4, [11]=5, [14]=6, [18]=7}
-- Give piece type, then piece orient and get the internal piece id. Starts with the spawn orientation and goes ccw from there
getId = {
  {2, 1, 0, 3},
  {7, 6, 5, 4},
  {8, 9},
  {10},
  {11, 12},
  {14, 13, 16, 15},
  {18, 17}
}

-- Reset all variables whose values are tied to one piece
function resetPieceScopedVars()
  adjustmentApiResult = nil
  framesUntilAdjustment = maxFramesUntilAdjustment
  framesUntilNextShift = 0
  pendingInputs = { left=0, right=0, A=0, B=0 }
  shiftsExecuted = 0
  rotationsExecuted = 0
end

-- Where to put the fm2 files. Replace with an absolute path. to format movieName go to initSeedData
recordGames = true
moviePath = "C:\\Users\\Greg\\Desktop\\AI"
movieName = "TestMovie3"

-- Global state
gameState = 0
playstate = 0
numLines = 0

maxDas = 3 -- the ARR minus 1
maxFramesUntilAdjustment = 12
waitingOnAsyncRequest = false
gameOver = false
pcur = 0
pnext = 0

-- Helper function because BCD is annoying
function toDec(a)
    return 10 * (a - (a % 16)) / 16 + (a % 16)
end

 -- This is where the board memory is accessed. Unfortunately lua is dumb so this table is 1 indexed (but stuff kept in memory is still 0 indexed :/)
function getBoard()
  local levelMap = {}
  for i=1,20 do
    levelMap[i] = {}
    for j=1,10 do
      levelMap[i][j] = memory.readbyte(4 * 256 + 10 * (i - 1) + (j - 1))
    end
  end
  return levelMap
end

--[[------------------------------------ 
----------- HTTP Requests -------------- 
------------------------------------]]--

function getEncodedBoard()
  local board = getBoard()
  local encodedStr = ""
  for _, row in ipairs(board) do
    for _, value in ipairs(row) do
      if value == 239 then
        encodedStr = encodedStr .. "0"
      else
        encodedStr = encodedStr .. "1"
      end
    end
  end
  return encodedStr
end

-- Make a request that will kick off a longer calculation. Subsequent frames will call checkForAsyncResult() to get the result.
function requestPlacementAsync()
  -- Format URL arguments
  local requestStr = "http://localhost:3000/async-nb/" .. getEncodedBoard()
  requestStr = requestStr .. "/" .. orientToPiece[pcur] .. "/" .. orientToPiece[pnext] 
  requestStr = requestStr .. "/" .. level .. "/" .. numLines

  waitingOnAsyncRequest = true
  return makeHttpRequest(requestStr).data
end

function checkForAsyncResult()
  local response = makeHttpRequest("http://localhost:3000/async-result")

  -- Only use the response if the server indicated that it sent the async result
  if response.code == 200 then
    adjustmentApiResult = response.data
    waitingOnAsyncRequest = false
  end
end

function requestPlacementSyncNoNextBox()
  -- Format URL arguments
  local requestStr = "http://localhost:3000/sync-nnb/" .. getEncodedBoard()
  local requestStr = requestStr .. "/" .. orientToPiece[pcur] .. "/null/" .. level .. "/" .. numLines

  return makeHttpRequest(requestStr).data
end

function makeHttpRequest(requestUrl)
  -- Helper function to compile the body of the web response
  local data = ""
  local function collect(chunk)
    if chunk ~= nil then
      data = data .. chunk
    end
    return true
  end

  local ok, statusCode, headers, statusText = http.request {
    method = "GET",
    url = requestUrl,
    sink = collect
  }
  return {data=data, code=statusCode}
end

function strsplit (inputstr, sep)
  if sep == nil then
          sep = "%s"
  end
  local t={}
  for str in string.gmatch(inputstr, "([^"..sep.."]+)") do
          table.insert(t, str)
  end
  return t
end

--[[------------------------------------ 
---------- Handling Input -------------- 
------------------------------------]]--

function calculateInputs(apiResult)
  if apiResult == "No legal moves" then
    return
  end

  -- Parse the shifts and rotations from the API result
  local split = strsplit(apiResult, ",")
  -- Offset by the amount of any existing inputs
  local numShifts = tonumber(split[2]) - shiftsExecuted
  local numRightRotations = (tonumber(split[1]) - rotationsExecuted) % 4

  pendingInputs = { left = 0, right = 0, A = 0, B = 0 }
  -- Shifts
  if numShifts < 0 then
    pendingInputs.left = -1 * numShifts
  elseif numShifts > 0 then
    pendingInputs.right = numShifts
  end

  -- Rotations
  if numRightRotations == 3 then
    pendingInputs.B = 1
  else
    pendingInputs.A = numRightRotations
  end
end

function executeInputs()
  if not gameOver then
    -- Either perform adjustment or decrement the adjustment countdown
    if framesUntilAdjustment == 0 then
      calculateInputs(adjustmentApiResult)
      framesUntilAdjustment = -1
    elseif framesUntilAdjustment > 0 then
      framesUntilAdjustment = framesUntilAdjustment - 1
    end

    local inputsThisFrame = {A=false, B=false, left=false, right=false, up=false, down=false, select=false, start=false}
    local stuckAgainstWall = shiftsExecuted == -5 or shiftsExecuted == 4
    if framesUntilNextShift == 0 then
      -- Execute inputs on this frame
      if pendingInputs.A > 0 and not stuckAgainstWall then
        inputsThisFrame.A = true
        pendingInputs.A = pendingInputs.A - 1 -- (Imagine having a decrement operator in your language)
        rotationsExecuted = (rotationsExecuted + 1) % 4
      end
      if pendingInputs.B > 0 and not stuckAgainstWall then
        inputsThisFrame.B = true
        pendingInputs.B = pendingInputs.B - 1
        rotationsExecuted = (rotationsExecuted - 1) % 4
      end
      if pendingInputs.left > 0 then
        inputsThisFrame.left = true
        pendingInputs.left = pendingInputs.left - 1
        shiftsExecuted = shiftsExecuted - 1
      end
      if pendingInputs.right > 0 then
        inputsThisFrame.right = true
        pendingInputs.right = pendingInputs.right - 1
        shiftsExecuted = shiftsExecuted + 1
      end

      -- Reset das frame count to max
      framesUntilNextShift = maxDas
    else
      framesUntilNextShift = framesUntilNextShift - 1
    end

    -- Send our computed inputs to the controller
    joypad.set(1, inputsThisFrame)
  end
end

--[[------------------------------------ 
------- Performance Monitoring  -------- 
------------------------------------]]--

-- Monitors the number of frames run per real clock second
function getMs()
  return socket.gettime()*1000
end

framesElapsed = 0
secsElapsed = 0
startTime = getMs()

function trackAndLogFps()
  framesElapsed = framesElapsed + 1
  local msElapsed = getMs() - startTime
  if msElapsed > (secsElapsed + 1) * 1000 then
    secsElapsed = secsElapsed + 1
    print("Average FPS:" .. framesElapsed / secsElapsed)
  end
end


--[[------------------------------------ 
---------- Main Game Loop  ------------- 
------------------------------------]]--


function onFirstFrameOfNewPiece()
  -- Read values from memory
  pcur = memory.readbyte(0x0042) -- Stores current/next pieces before they even appear onscreen
  pnext = memory.readbyte(0x0019)
  numLines = toDec(memory.readbyte(0x0051)) * 100 + toDec(memory.readbyte(0x0050))
  level = memory.readbyte(0x0044)
  
  resetPieceScopedVars()
  
  if not gameOver then
    -- Make a synchronous request to the server for the inital placement
    local apiResult = requestPlacementSyncNoNextBox()
    calculateInputs(apiResult)
  end
end

function beforeEachFrame()
  --Game starts
  if(gameState == 3 and memory.readbyte(0x00C0) == 4) then
    if(recordGames) then
      movie.record(moviePath .. movieName .. ".fm2", 1, "gregcannon")
    end
  end

  --Check if a reset, hard reset, save state(?) has been loaded.
  if(gameState == 4 and memory.readbyte(0x00C0) < 3) then
  -- Panic
  end

  --Game ends, clean up data
  if(gameState == 4 and memory.readbyte(0x00C0) == 3) then
  gameOver = false
  framesUntilNextShift = 0
  if movie.active() then
      movie.stop()
      end
  movieName = ""
  end

  --Update gameState
  gameState = memory.readbyte(0x00C0)

  if(gameState < 4) then
    -- Currently on menu
  end

  if(gameState == 4) then
    if(memory.readbyte(0x0048) == 1) then
      if(playstate ~= 1 or backtrack) then
        -- First active frame for piece. This is where board state/input sequence is calculated
        onFirstFrameOfNewPiece()
      else
        -- Subsequent frames where the piece is active
        if waitingOnAsyncRequest then
          checkForAsyncResult()
        elseif adjustmentApiResult == nil and not waitingOnAsyncRequest then
          requestPlacementAsync()
          waitingOnAsyncRequest = true
        end
      end

      -- Execute input sequence
      executeInputs()

    -- Do stuff right when the piece locks. If you want to check that the piece went to the correct spot/send an API request early here is probably good.
    elseif(memory.readbyte(0x0048) == 2 and playstate == 1) then     
    
    -- Detects when the game is over.
    elseif memory.readbyte(0x0048) == 10 then
        gameOver = true
    
    -- Resets the index for the next piece. Disables user input when the game is not over.
    elseif not gameOver or recording then
      pendingInputs = { left=0, right=0, A=0, B=0 }
      joypad.set(1, {A=false,B=false,left=false,right=false,up=false,down=false,select=false,start=false})
    end
  end

  playstate = memory.readbyte(0x0048)
  trackAndLogFps()
end

emu.registerbefore(beforeEachFrame)