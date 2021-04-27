local http = require("socket.http")
local os = require("os")
require "socket"

TIMELINE_12_HZ = "X....";
TIMELINE_13_HZ = "X....X...";
TIMELINE_13_5_HZ = "X....X...X...";
TIMEILNE_14_HZ = "X....X...X...X...";
TIMELINE_15_HZ = "X...";
TIMELINE_20_HZ = "X..";
TIMELINE_30_HZ = "X.";

-- Config constants
REACTION_TIME_FRAMES = 5
INPUT_TIMELINE = TIMELINE_30_HZ;
SHOULD_RECORD_GAMES = true
MOVIE_PATH = "C:\\Users\\Greg\\Desktop\\VODs\\" -- Where to store the fm2 VODS (absolute path)

-- Global state
gameState = 0
playstate = 0
numLines = 0
waitingOnAsyncRequest = false
gameOver = false
pcur = 0
pnext = 0

-- Reset all variables whose values are tied to one piece
function resetPieceScopedVars()
  adjustmentApiResult = nil
  frameIndex = 0
  pendingInputs = { left=0, right=0, A=0, B=0 }
  shiftsExecuted = 0
  rotationsExecuted = 0
end

--[[--------------------------------------- 
------------ Helper Functions ------------- 
---------------------------------------]]--

-- Translate internal Piece IDs to actual piece types (T: 2 J: 7 Z: 9 O: 10 S: 11 L: 15 I: 18)
orientToPiece = {[0]="none", [2]="T", [7]="J", [8]="Z", [10]="O", [11]="S", [14]="L", [18]="I"}
orientToNum = {[0]="none", [2]=1, [7]=2, [8]=3, [10]=4, [11]=5, [14]=6, [18]=7}

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

function getGravity(level)
  if level <= 8 then
    return 8 -- We don't really care about hyper-optimizing low levels
  elseif level == 9 then
    return 6
  elseif level <= 12 then
    return 5
  elseif level <= 15 then
    return 4
  elseif level <= 18 then
    return 3
  elseif level <= 28 then
    return 2
  else
    return 1
  end
end

-- Based on the current placement, predict exactly where the piece will be when it's time to adjust it
function predictPieceOffsetAtAdjustmentTime()
  local shiftsCompletedByAdjTime = 0
  offsetXAtAdjustmentTime = 0
  offsetYAtAdjustmentTime = 0
  for i = 0, REACTION_TIME_FRAMES-1 do
    if (isInputFrame(i)) then
      shiftsCompletedByAdjTime = shiftsCompletedByAdjTime + 1
    end
  end

  offsetYAtAdjustmentTime = math.floor((REACTION_TIME_FRAMES) / getGravity(level))

  -- Calculate how many of the pending shifts it will have completed by that point
  offsetXAtAdjustmentTime = 0
  if pendingInputs.left > 0 then
    offsetXAtAdjustmentTime = -1 * math.min(shiftsCompletedByAdjTime, pendingInputs.left)
  elseif pendingInputs.right > 0 then
    offsetXAtAdjustmentTime = math.min(shiftsCompletedByAdjTime, pendingInputs.right)
  end

  -- Calculate how many of the pending rotations it will have completed by that point
  rotationAtAdjustmentTime = 0
  if pendingInputs.B == 1 and shiftsCompletedByAdjTime >= 1 then
    rotationAtAdjustmentTime = 3
  elseif pendingInputs.A > 0 then
    rotationAtAdjustmentTime = math.min(shiftsCompletedByAdjTime, pendingInputs.A)
  end
end


--[[------------------------------------ 
----------- HTTP Requests -------------- 
------------------------------------]]--

-- Make a request that will kick off a longer calculation. Subsequent frames will call checkForAsyncResult() to get the result.
function requestPlacementAsync()
  -- Format URL arguments
  local requestStr = "http://localhost:3000/async-nb/" .. getEncodedBoard()
  requestStr = requestStr .. "/" .. orientToPiece[pcur] .. "/" .. orientToPiece[pnext] .. "/" .. level .. "/" .. numLines
  requestStr = requestStr .. "/" .. offsetXAtAdjustmentTime .. "/" .. offsetYAtAdjustmentTime .. "/" .. rotationAtAdjustmentTime
  requestStr = requestStr .. "/" .. REACTION_TIME_FRAMES .. "/" .. INPUT_TIMELINE .. "/true"

  waitingOnAsyncRequest = true
  return makeHttpRequest(requestStr).data
end

-- Check if the async computation has finished, and if so make the adjustment based on it
function checkForAsyncResult()
  local response = makeHttpRequest("http://localhost:3000/async-result")

  -- Only use the response if the server indicated that it sent the async result
  if response.code == 200 then
    adjustmentApiResult = response.data
    print("Adjustment: " .. adjustmentApiResult)
    waitingOnAsyncRequest = false
  end
end

-- Synchronously get a placement from the server, with no next piece data
function requestPlacementSyncNoNextBox()
  -- Format URL arguments
  local requestStr = "http://localhost:3000/sync-nnb/" .. getEncodedBoard()
  local requestStr = requestStr .. "/" .. orientToPiece[pcur] .. "/null/" .. level .. "/" .. numLines
  requestStr = requestStr .. "/0/0/0/0/" .. INPUT_TIMELINE .. "/false"

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

-- Implementation of string split that I definitely didn't find on stack overflow
function splitString (inputstr, sep)
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
  local split = splitString(apiResult, ",")
  -- Offset by the amount of any existing inputs
  local numShifts = tonumber(split[2])
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

  print(pendingInputs)
end

function isInputFrame(index)
  local len = string.len(INPUT_TIMELINE)
  local strIndex = index % len + 1; -- IMAGINE 1-indexing!
  return string.sub(INPUT_TIMELINE, strIndex, strIndex) == "X"
end

function executeInputs()
  if not gameOver then
    -- Either perform adjustment or decrement the adjustment countdown
    if frameIndex == REACTION_TIME_FRAMES and adjustmentApiResult ~= nil then
      if shiftsExecuted ~= offsetXAtAdjustmentTime then
        print("Actual X offset: " .. shiftsExecuted .. " predicted: " .. offsetXAtAdjustmentTime .. " Diff: " .. offsetXAtAdjustmentTime - shiftsExecuted)
      end
      calculateInputs(adjustmentApiResult)
    end

    local inputsThisFrame = {A=false, B=false, left=false, right=false, up=false, down=false, select=false, start=false}
    -- local stuckAgainstWall = shiftsExecuted == -5 or shiftsExecuted == 4 -- Can't rotate due to NES's lack of kick functionality
    local stuckAgainstWall = false;

    -- print(emu.framecount() .. "   " .. frameIndex .. "   " .. tostring(isInputFrame(frameIndex)))
    if isInputFrame(frameIndex) then
      local function execute(inputName)
        inputsThisFrame[inputName] = true
        pendingInputs[inputName] = pendingInputs[inputName] - 1  -- Imagine having a decrement operator in your language
      end

      -- Execute one rotation if any pending
      if pendingInputs.A > 0 and not stuckAgainstWall then
        execute("A")
        rotationsExecuted = (rotationsExecuted + 1) % 4
      elseif pendingInputs.B > 0 and not stuckAgainstWall then
        execute("B")
        rotationsExecuted = (rotationsExecuted - 1) % 4
      end
      -- Execute one shift if any pending
      if pendingInputs.left > 0 then
        inputsThisFrame.left = true
        pendingInputs.left = pendingInputs.left - 1
        shiftsExecuted = shiftsExecuted - 1
      elseif pendingInputs.right > 0 then
        inputsThisFrame.right = true
        pendingInputs.right = pendingInputs.right - 1
        shiftsExecuted = shiftsExecuted + 1
      end
    end

    -- Debug logs
    if inputsThisFrame.left or inputsThisFrame.right then
      print("SHIFT" .. emu.framecount())
    elseif pendingInputs.left > 0 or pendingInputs.right > 0 or pendingInputs.A > 0 or pendingInputs.B > 0 then
      print("has pending inputs: " .. pendingInputs.left .. pendingInputs.right .. pendingInputs.B .. pendingInputs.A)
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
    if secsElapsed % 30 == 0 then
      print("Average FPS:" .. framesElapsed / secsElapsed)
    end
  end
end


--[[------------------------------------ 
---------- Main Game Loop  ------------- 
------------------------------------]]--

function runGameFrame()
  if(memory.readbyte(0x0048) == 1) then
    if(playstate ~= 1 or backtrack) then
      -- First active frame for piece. This is where board state/input sequence is calculated
      onFirstFrameOfNewPiece()

      -- Initiate a request for good adjustments
      predictPieceOffsetAtAdjustmentTime()
      requestPlacementAsync()
      waitingOnAsyncRequest = true
    else
      -- Subsequent frames where the piece is active
      if waitingOnAsyncRequest then
        checkForAsyncResult()
      end
    end

    -- Execute input sequence
    executeInputs()
    frameIndex = frameIndex + 1

  -- Do stuff right when the piece locks. If you want to check that the piece went to the correct spot/send an API request early here is probably good.
  elseif(memory.readbyte(0x0048) == 2 and playstate == 1) then     
    print("pieceLock" .. emu.framecount())
  -- Detects when the game is over.
  elseif memory.readbyte(0x0048) == 10 then
      gameOver = true

  -- Resets the index for the next piece. Disables user input when the game is not over.
  elseif not gameOver or recording then
    pendingInputs = { left=0, right=0, A=0, B=0 }
    joypad.set(1, {A=false,B=false,left=false,right=false,up=false,down=false,select=false,start=false})
  end
end

function onFirstFrameOfNewPiece()
  -- Read values from memory
  local function bcdToDecimal(a)
    return 10 * (a - (a % 16)) / 16 + (a % 16)
  end
  pcur = memory.readbyte(0x0042) -- Stores current/next pieces before they even appear onscreen
  pnext = memory.readbyte(0x0019)
  numLines = bcdToDecimal(memory.readbyte(0x0051)) * 100 + bcdToDecimal(memory.readbyte(0x0050))
  level = memory.readbyte(0x0044)
  
  print("------" .. orientToPiece[pcur] .. "------")

  resetPieceScopedVars()
  
  if not gameOver then
    -- Make a synchronous request to the server for the inital placement
    local apiResult = requestPlacementSyncNoNextBox()
    if apiResult == "" then
      print("ERROR - backend not connected!")
    end
    print("Initial placement: " .. apiResult)
    calculateInputs(apiResult)
  end
end


--[[-------------------------------------------------------- 
---------- Non-Gameplay Per-Frame Calculations ------------- 
--------------------------------------------------------]]--


function beforeEachFrame()
  --Game starts
  if(gameState == 3 and memory.readbyte(0x00C0) == 4) then
    if(SHOULD_RECORD_GAMES) then
      local dateStr = os.date("%m-%d %H %M")
      print(dateStr)
      movieName = "StackRabbit" .. dateStr
      print(movieName)
      movie.record(MOVIE_PATH .. movieName .. ".fm2", 1, "gregcannon")
    end
  end

  --Check if a reset, hard reset, save state(?) has been loaded.
  if(gameState == 4 and memory.readbyte(0x00C0) < 3) then
  -- Panic
  end

  --Game ends, clean up data
  if(gameState == 4 and memory.readbyte(0x00C0) == 3) then
  gameOver = false
  if movie.active() then
      movie.stop()
      end
  end

  --Update gameState
  gameState = memory.readbyte(0x00C0)

  if(gameState < 4) then
    -- Currently on menu
  end

  if(gameState == 4) then
    runGameFrame()
  end

  playstate = memory.readbyte(0x0048)
  trackAndLogFps()
end

emu.registerafter(beforeEachFrame)