local http = require("socket.http")
local os = require("os")
require "socket"

TIMELINE_10_HZ = "X.....";
TIMELINE_12_HZ = "X....";
TIMELINE_13_HZ = "X....X...";
TIMELINE_13_5_HZ = "X....X...X...";
TIMEILNE_14_HZ = "X....X...X...X...";
TIMELINE_15_HZ = "X...";
TIMELINE_20_HZ = "X..";
TIMELINE_30_HZ = "X.";

-- Config constants
REACTION_TIME_FRAMES = 15
INPUT_TIMELINE = TIMELINE_12_HZ;
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
  arrFrameIndex = 0
  inputSequence = ""
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
  if level >= 29 then
    return 1
  elseif level >= 19 then
    return 2
  elseif level >= 16 then
    return 3
  elseif level >= 13 then
    return 4
  elseif level >= 10 then
    return 5
  elseif level == 9 then
    return 6
  elseif level == 8 then
    return 8
  elseif level == 7 then
    return 13
  elseif level == 6 then
    return 18
  elseif level == 5 then
    return 23
  elseif level == 4 then
    return 28
  elseif level == 3 then
    return 33
  elseif level == 2 then
    return 38
  elseif level == 1 then
    return 43
  elseif level == 0 then
    return 48
  else
    error("Unknown level" .. level)
  end
end

-- Query into the input sequence based on (0-indexed) arrFrameIndex
function getInputForFrame(index)
  return string.sub(inputSequence, index + 1, index + 1)
end

-- Based on the current placement, predict exactly where the piece will be when it's time to adjust it
function predictPieceOffsetAtAdjustmentTime()
  local inputsPossibleByAdjTime = 0
  local inputsUsedByAdjTime = 0
  offsetXAtAdjustmentTime = 0
  rotationAtAdjustmentTime = 0

  -- Loop through the frames until adjustment time and track the input sequence, gravity, and possible number of shifts
  for i = 0, REACTION_TIME_FRAMES-1 do
    if (isInputFrame(i)) then
      inputsPossibleByAdjTime = inputsPossibleByAdjTime + 1
    end
    
    -- Track shifts
    local thisFrameStr = getInputForFrame(i)
    -- print("thisFrameStr " .. thisFrameStr)
    if thisFrameStr == "L" or thisFrameStr == "e" or thisFrameStr == "f" then
      offsetXAtAdjustmentTime = offsetXAtAdjustmentTime - 1
    elseif thisFrameStr == "R" or thisFrameStr == "i" or thisFrameStr == "g" then
      offsetXAtAdjustmentTime = offsetXAtAdjustmentTime + 1
    end

    -- Track rotations
    if thisFrameStr == "A" or thisFrameStr == "e" or thisFrameStr == "i" then
      rotationAtAdjustmentTime = rotationAtAdjustmentTime + 1
    elseif thisFrameStr == "B" or thisFrameStr == "f" or thisFrameStr == "g" then
      rotationAtAdjustmentTime = rotationAtAdjustmentTime - 1
    end

    -- Track inputs used
    if thisFrameStr ~= "." then
      inputsUsedByAdjTime = inputsUsedByAdjTime + 1
    end
  end

  rotationAtAdjustmentTime = (rotationAtAdjustmentTime + 4) % 4 -- Modulus -1 to 3, etc.
  offsetYAtAdjustmentTime = math.floor((REACTION_TIME_FRAMES) / getGravity(level))

  -- -- Calculate how many of the pending shifts it will have completed by that point
  -- offsetXAtAdjustmentTime = 0
  -- if pendingInputs.left > 0 then
  --   offsetXAtAdjustmentTime = -1 * math.min(inputsPossibleByAdjTime, pendingInputs.left)
  -- elseif pendingInputs.right > 0 then
  --   offsetXAtAdjustmentTime = math.min(inputsPossibleByAdjTime, pendingInputs.right)
  -- end

  -- -- Calculate how many of the pending rotations it will have completed by that point
  -- rotationAtAdjustmentTime = 0
  -- if pendingInputs.B == 1 and inputsPossibleByAdjTime >= 1 then
  --   rotationAtAdjustmentTime = 3
  -- elseif pendingInputs.A > 0 then
  --   rotationAtAdjustmentTime = math.min(inputsPossibleByAdjTime, pendingInputs.A)
  -- end

  -- Calculate if it can first-frame shift at adjustment time
  canFirstFrameShiftAtAdjustmentTime = inputsUsedByAdjTime < inputsPossibleByAdjTime
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
  requestStr = requestStr .. "/" .. REACTION_TIME_FRAMES .. "/" .. INPUT_TIMELINE .. "/" .. tostring(canFirstFrameShiftAtAdjustmentTime)

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
  print(requestUrl)
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

function calculateInputs(apiResult, isAdjustment)
  if apiResult == "No legal moves" then
    return
  end

  -- Parse the shifts and rotations from the API result
  local split = splitString(apiResult, ",|\\")
  inputSequence = split[3]
  print(inputSequence)
  if inputSequence == nil then
    inputSequence = ""
  end

  -- -- Offset by the amount of any existing inputs
  -- local numShifts = tonumber(split[2])
  -- local numRightRotations = (tonumber(split[1]) - rotationsExecuted) % 4

  -- pendingInputs = { left = 0, right = 0, A = 0, B = 0 }
  -- -- Shifts
  -- if numShifts < 0 then
  --   pendingInputs.left = -1 * numShifts
  -- elseif numShifts > 0 then
  --   pendingInputs.right = numShifts
  -- end

  -- -- Rotations
  -- if numRightRotations == 3 then
  --   pendingInputs.B = 1
  -- else
  --   pendingInputs.A = numRightRotations
  -- end

  -- Reset ARR counter if is an adjustment and can first-frame shift
  if isAdjustment then
    arrFrameIndex = 0
  end

  -- print(pendingInputs)
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
      print("Time for adjustment" .. frameIndex .. ", " .. arrFrameIndex)
      calculateInputs(adjustmentApiResult, true)
    end

    local inputsThisFrame = {A=false, B=false, left=false, right=false, up=false, down=false, select=false, start=false}

    if inputSequence == null or arrFrameIndex + 1 > string.len(inputSequence) then
      -- print("Input sequence null or frame index out of bounds" .. arrFrameIndex)
      -- print(inputSequence)
      return
    end

    local thisFrameStr = getInputForFrame(arrFrameIndex);
    print(arrFrameIndex .. thisFrameStr)
    -- Simple cases
    if thisFrameStr == "A" then
      inputsThisFrame.A = true;
    elseif thisFrameStr == "B" then
      inputsThisFrame.B = true;
    elseif thisFrameStr == "L" then
      inputsThisFrame.left = true;
    elseif thisFrameStr == "R" then
      inputsThisFrame.right = true;
    -- Combo cases
    elseif thisFrameStr == "e" then
      inputsThisFrame.left = true;
      inputsThisFrame.A = true;
    elseif thisFrameStr == "f" then
      inputsThisFrame.left = true;
      inputsThisFrame.B = true;
    elseif thisFrameStr == "i" then
      inputsThisFrame.right = true;
      inputsThisFrame.A = true;
    elseif thisFrameStr == "g" then
      inputsThisFrame.right = true;
      inputsThisFrame.B = true;
    elseif thisFrameStr == "." then
      -- Do nothing
    else
      print("Unknown character in input sequence" .. arrFrameIndex + 1)
      print(thisFrameStr)
    end

    if inputsThisFrame.left then
      shiftsExecuted = shiftsExecuted - 1
    elseif inputsThisFrame.right then
      shiftsExecuted = shiftsExecuted + 1
    end

    -- -- print(emu.framecount() .. "   " .. arrFrameIndex .. "   " .. tostring(isInputFrame(arrFrameIndex)))
    -- if isInputFrame(arrFrameIndex) then
    --   local function execute(inputName)
    --     inputsThisFrame[inputName] = true
    --     pendingInputs[inputName] = pendingInputs[inputName] - 1  -- Imagine having a decrement operator in your language
    --   end

    --   -- Execute one rotation if any pending
    --   if pendingInputs.A > 0 then
    --     execute("A")
    --     rotationsExecuted = (rotationsExecuted + 1) % 4
    --   elseif pendingInputs.B > 0 then
    --     execute("B")
    --     rotationsExecuted = (rotationsExecuted - 1) % 4
    --   end
    --   -- Execute one shift if any pending
    --   if pendingInputs.left > 0 then
    --     execute("left")
    --     shiftsExecuted = shiftsExecuted - 1
    --   elseif pendingInputs.right > 0 then
    --     execute("right")
    --     shiftsExecuted = shiftsExecuted + 1
    --   end
    -- end

    -- Debug logs
    if inputsThisFrame.left or inputsThisFrame.right then
      print("SHIFT" .. emu.framecount())
    else
      -- print("..." .. emu.framecount())
    end
    -- elseif pendingInputs.left > 0 or pendingInputs.right > 0 or pendingInputs.A > 0 or pendingInputs.B > 0 then
    --   print("has pending inputs: " .. pendingInputs.left .. pendingInputs.right .. pendingInputs.B .. pendingInputs.A)
    -- end

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
    arrFrameIndex = arrFrameIndex + 1

  -- Do stuff right when the piece locks. If you want to check that the piece went to the correct spot/send an API request early here is probably good.
  elseif(memory.readbyte(0x0048) == 2 and playstate == 1) then     
    print("pieceLock" .. emu.framecount())
  -- Detects when the game is over.
  elseif memory.readbyte(0x0048) == 10 then
      gameOver = true

  -- Resets the index for the next piece. Disables user input when the game is not over.
  elseif not gameOver or recording then
    -- pendingInputs = { left=0, right=0, A=0, B=0 }
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
  
  resetPieceScopedVars()
  
  print("--------------------")
  print(orientToPiece[pcur])

  if not gameOver then
    -- Make a synchronous request to the server for the inital placement
    local apiResult = requestPlacementSyncNoNextBox()
    if apiResult == "" then
      print("ERROR - backend not connected!")
    end
    print("Initial placement: " .. apiResult)
    calculateInputs(apiResult, false)
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