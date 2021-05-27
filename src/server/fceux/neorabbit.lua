local http = require("socket.http")
local os = require("os")
require "socket"

-- Constants
STARTUP_FRAMES = 16
FIRST_PIECE_TOTAL_DELAY = 99
GameState = {IN_GAME=1, MENU=2, GAME_OVER=3}
MOVIE_PATH = "C:\\Users\\Greg\\Desktop\\VODs\\" -- Where to store the fm2 VODS (absolute path)
TIMELINE_2_HZ = "X.............................";
TIMELINE_6_HZ = "X........";
TIMELINE_7_HZ = "X.......";
TIMELINE_8_HZ = "X......";
TIMELINE_10_HZ = "X.....";
TIMELINE_11_HZ = "X.....X....X....";
TIMELINE_12_HZ = "X....";
TIMELINE_13_HZ = "X....X...";
TIMELINE_13_5_HZ = "X....X...X...";
TIMEILNE_14_HZ = "X....X...X...X...";
TIMELINE_15_HZ = "X...";
TIMELINE_20_HZ = "X..";
TIMELINE_30_HZ = "X.";
TIMELINE_KYROS = "......X.X.X.X.X.X.X.X.X"

-- Configurable Params
STARTING_LEVEL = 19
REACTION_TIME_FRAMES = 18
REACTION_IS_ARTIFICIAL = true -- True if it's a handicap for adjustments, False if it's a hardware limitation
INPUT_TIMELINE = TIMELINE_12_HZ;
SHOULD_RECORD_GAMES = true

-- Global Variables
g_menuFrameIndex = 0

--[[------------------------------------ 
----------- HTTP Requests -------------- 
------------------------------------]]--

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


-- Check if the async call has finished, and if so returns the computation result
function fetchAsyncResult()
  local response = makeHttpRequest("http://localhost:8080/async-result")

  -- Only use the response if the server indicated that it sent the async result
  if response.code ~= 200 then
    error("RECEIVED BAD RESPONSE CODE:" .. response.code)
    return nil
  end 
  n_waitingOnAsyncRequest = false;
  print(response.data)
  return response.data
end



function requestPrecompute(isForFirstPiece)
  print("requestprecompute")
  -- Format URL arguments
  if n_stateForNextPiece == nil or n_stateForNextPiece.board == nil
        or n_stateForNextPiece.lines == nil or n_stateForNextPiece.level == nil then
    error("Unknown state for next piece")
  end

  local pieceStr = ternary(isForFirstPiece, n_currentPiece, n_nextPiece)
  local framesElapsed = ternary(isForFirstPiece, 0, REACTION_TIME_FRAMES)

  local requestStr = "http://localhost:8080/precompute/" .. n_stateForNextPiece.board
  local requestStr = requestStr .. "/" .. pieceStr .. "/null/" .. n_stateForNextPiece.level
  local requestStr = requestStr .. "/" .. n_stateForNextPiece.lines .. "/0/0/0/"
  local requestStr = requestStr .. framesElapsed .. "/" .. INPUT_TIMELINE .. "/false" -- use the 'framesAlreadyElapsed' param to communicate reaction time

  local response = makeHttpRequest(requestStr)
  if response.code ~= 200 then
    error("Request not acknowledged by backend")
  end
  n_waitingOnAsyncRequest = true; 
end


function processPrecomputeResult(precomputeResult)
  local rows = splitString(precomputeResult, "\n")

  -- Parse the initial placement (if it has a reaction time) and queue up those inputs
  if REACTION_TIME_FRAMES > 0 or n_isFirstPiece then
    local defaultPlacement = splitString(rows[1], ":")[2]
    if defaultPlacement == null then
      print("GAME OVER - no default placement")
      n_gameState = GameState.GAME_OVER
      return
    end
    print("Initial placement: " .. defaultPlacement)
    queueUpInputs(defaultPlacement, --[[isAdjustment]] false)
    parseGameStateFromResponse(defaultPlacement)
  end

  -- Store all the adjustments in a lookup table
  for i = 2,8 do
    local resultSplit = splitString(rows[i], ":")
    local pieceStr = resultSplit[1]
    n_adjustmentLookup[pieceStr] = resultSplit[2]
  end
end


function parseGameStateFromResponse(apiResult)
  if apiResult == "No legal moves" or apiResult == nil then
    return
  end

  local split = splitString(apiResult, ",|\|")
  
  if split[4] ~= nil and split[5] ~= nil and split[6] ~= nil then
    n_stateForNextPiece = { 
      board=split[4], 
      level=split[5], 
      lines=split[6] 
    }
  end
end


function processAdjustment()
  if (n_adjustmentLookup == {}) then
    error("No adjustment lookup found")
  end
  print("Time for adjustment " .. n_pieceFrameIndex)

  local adjustmentApiResult = n_adjustmentLookup[n_nextPiece]
  queueUpInputs(adjustmentApiResult, --[[isAdjustment]] true)
  parseGameStateFromResponse(adjustmentApiResult)
   
end


--[[------------------------------------ 
----------- Input Handling ------------- 
------------------------------------]]--


function queueUpInputs(apiResult, isAdjustment)
  if apiResult == "No legal moves" or apiResult == nil then
    return
  end

  -- Parse the input sequence from the API result
  local split = splitString(apiResult, ",|\|")
  inputSequence = split[3]

  if inputSequence == nil or inputSequence == "none" then
    return
  end

  -- if not isAdjustment then
  --   inputSequence = inputSequence:sub(1,REACTION_TIME_FRAMES)
  -- end
  if isAdjustment then
    n_frameQueue = {} -- Wipe the previous placement
  end

  print("QUEUEING UP:" .. inputSequence)

  -- Add each input to the frame queue
  for i = 1, #inputSequence do
    local inputChar = inputSequence:sub(i,i)
    table.insert(n_frameQueue, inputChar)
  end
end



function executeInputs(thisFrameStr)
  local controllerInputs = {A=false, B=false, left=false, right=false, up=false, down=false, select=false, start=false}

  print(n_pieceFrameIndex .. "  " .. thisFrameStr)
  -- Simple cases
  if thisFrameStr == "A" then
    controllerInputs.A = true;
  elseif thisFrameStr == "B" then
    controllerInputs.B = true;
  elseif thisFrameStr == "L" then
    controllerInputs.left = true;
  elseif thisFrameStr == "R" then
    controllerInputs.right = true;
  -- Combo cases
  elseif thisFrameStr == "E" then
    controllerInputs.left = true;
    controllerInputs.A = true;
  elseif thisFrameStr == "F" then
    controllerInputs.left = true;
    controllerInputs.B = true;
  elseif thisFrameStr == "I" then
    controllerInputs.right = true;
    controllerInputs.A = true;
  elseif thisFrameStr == "G" then
    controllerInputs.right = true;
    controllerInputs.B = true;
  elseif thisFrameStr == "." or thisFrameStr == "*" or thisFrameStr == "^" then
    -- Do nothing
  else
    error("Unknown character in input sequence" .. thisFrameStr)
  end

  -- Send our computed inputs to the controller
  joypad.set(1, controllerInputs)
end


function fetchCurrentPiece()
  print("Fetching current piece")
  local orientToPiece = {[0]="none", [2]="T", [7]="J", [8]="Z", [10]="O", [11]="S", [14]="L", [18]="I"}
  pcur = memory.readbyte(0x0042) -- Stores current/next pieces before they even appear onscreen
  n_currentPiece = orientToPiece[pcur]
  print("CURRENT PIECE:" .. n_currentPiece)
end

function fetchNextPiece()
  print("Fetching next piece")
  local orientToPiece = {[0]="none", [2]="T", [7]="J", [8]="Z", [10]="O", [11]="S", [14]="L", [18]="I"}
  pnext = memory.readbyte(0x0019)
  n_nextPiece = orientToPiece[pnext]
  print("NEXT PIECE:" .. n_nextPiece)
end



--[[------------------------------------ 
----------- Major Frame Loops ------------ 
------------------------------------]]--

gamePhase = 0

function doubleCheck()
  if n_isFirstPiece then
    return
  end
  local gamePhaseLastFrame = gamePhase
  gamePhase = memory.readbyte(0x0048)
  if(gamePhase == 1 and gamePhaseLastFrame ~= 1) then
    -- First active frame for piece
    if table.getn(n_frameQueue) ~= 0 then
      for k,v in ipairs(n_frameQueue) do
        print(k .. "==>" .. v)
      end
      -- print("EXTRA ON PIECE SPAWN")
      error("Queue has extra inputs on piece spawn")
    end
  end
end


function runGameFrame()
  print("Game frame" .. n_frameIndex)
  doubleCheck()

  ---------------- Event handling -----------------

  -- Event: Reading first piece
  if n_frameIndex == STARTUP_FRAMES then
    fetchCurrentPiece() -- The only time we ever fetch the current piece!
    fetchNextPiece()
    requestPrecompute(--[[isForFirstPiece]] true)
  end

  -- Event: First piece comes into control
  if n_frameIndex == FIRST_PIECE_TOTAL_DELAY then
    n_pieceFrameIndex = 0

    -- Check in on the result of the async precompute
    local apiResult = fetchAsyncResult()
    processPrecomputeResult(apiResult)
    processAdjustment()

    -- Request precompute for the 2nd piece
    requestPrecompute(--[[isForFirstPiece]] false)
  end

  -- Event: First frame of piece
  --        (note that this is before adjustment handling so 0 ms agents can instantly adjust)
  if not n_isFirstPiece and table.getn(n_frameQueue) == 0 and n_previousFrame == "*" then
    n_pieceFrameIndex = 0

    if not n_waitingOnAsyncRequest then
      error("Piece spawned with no precompute ready")
    end

    -- Check in on the result of the async precompute
    local apiResult = fetchAsyncResult()
    processPrecomputeResult(apiResult)

    -- If the precompute says it's game over, don't do anything more
    if n_gameState == GameState.GAME_OVER then
      return
    end
  end

  -- Event: Reaction time ends
  if not n_isFirstPiece and n_pieceFrameIndex == REACTION_TIME_FRAMES then
    fetchNextPiece()
    processAdjustment()
    requestPrecompute(--[[isForFirstPiece]] false)
  end

  inputThisFrame = table.remove(n_frameQueue, 1)
  if inputThisFrame == "^" and n_previousFrame ~= "^" then
    -- Calculate extra line clear frames
    local predictedGfc = (n_gfcOffset + n_frameIndex) % 256
    if predictedGfc ~= memory.readbyte(0x00B1) then
      print(predictedGfc)
      print(memory.readbyte(0x00B1))
      error("Bad frame prediction")
    end

    gfc = predictedGfc % 4
    extraFrames = 0
    if gfc == 1 then
      extraFrames = 3
    elseif gfc == 2 then
      extraFrames = 2
    elseif gfc == 3 then
      extraFrames = 1
    end

    print("EXTRA FRAMES:", extraFrames)
    for i = 1,extraFrames do
      table.insert(n_frameQueue, 5, "^")
    end
  end

  -- Exit early if we don't have an active piece yet
  if (n_pieceFrameIndex == nil) then
    -- Get ready for next frame and quit out, since we can't do the rest of the function without an active piece
    n_frameIndex = n_frameIndex + 1
    n_previousFrame = inputThisFrame
    return
  end

  -- Event: Piece locks
  if inputThisFrame == "*" and n_previousFrame ~= nil and n_previousFrame ~= "*" then
    -- If we haven't 'seen' the next piece yet due to reaction time
    if not n_isFirstPiece and not n_waitingOnAsyncRequest then
      if REACTION_IS_ARTIFICIAL then
        -- If the reaction is just for limiting adjustments, rigger the adjustment step now
        processAdjustment()
        requestPrecompute(--[[isForFirstPiece]] false)
      else
        -- If we really don't know yet due to hardware limitations, just top out for now
        n_gameState = GameState.GAME_OVER
        n_frameIndex = n_frameIndex + 1
        return
      end
    end
    n_isFirstPiece = false
  end

  -- DEBUG Event: End of ARE
  if inputThisFrame == "*" and table.getn(n_frameQueue) == 1 then
    if getEncodedBoard() ~= n_stateForNextPiece.board then
      print(getEncodedBoard())
      print(n_stateForNextPiece.board)
      error("Divergence")
    end
  end
  
  ----------------- Standard inputs -----------------

  executeInputs(inputThisFrame)

  -------------- Get state ready for next frame -----------

  n_previousFrame = inputThisFrame
  n_frameIndex = n_frameIndex + 1
  n_pieceFrameIndex = n_pieceFrameIndex + 1
end


function runMenuFrame()
  if g_menuFrameIndex == 0 then
    joypad.set(1, {A=true, B=false, left=false, right=false, up=false, down=false, select=false, start=false})
  else
    joypad.set(1, {A=true, B=false, left=false, right=false, up=false, down=false, select=false, start=true})
    startGame()
  end

  g_menuFrameIndex = g_menuFrameIndex + 1
end


--[[------------------------------------ 
----------- Helper Functions ------------ 
------------------------------------]]--

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

function ternary (condition, T, F)
  if condition then return T else return F end
end

function startRecording()
  if(SHOULD_RECORD_GAMES) then
    local dateStr = os.date("%m-%d %H %M")
    movieName = "StackRabbit" .. dateStr
    print(movieName)
    movie.record(MOVIE_PATH .. movieName .. ".fm2", 1, "gregcannon")
  end
end

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
  local encodedStr = ""

  if isFirstPiece then
    for i=1,200 do
      encodedStr = encodedStr .. "0"
    end
    return encodedStr
  end

  local board = getBoard()
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

function getEmptyBoard()
encodedStr = ""
  for i=1,20 do
    encodedStr = encodedStr .. "0000000000"
  end
  return encodedStr
end

function startGame()
  n_gameState = GameState.IN_GAME
  n_frameIndex = 0
  n_frameQueue = {}

  n_previousFrame = nil
  n_pieceFrameIndex = nil
  n_waitingOnAsyncRequest = false
  n_stateForNextPiece = {
    board = getEmptyBoard(),
    level = STARTING_LEVEL,
    lines = 0
  }
  n_gfcOffset = memory.readbyte(0x00B1) + 1
  print("GFC OFFSET: ", n_gfcOffset)
  n_isFirstPiece = true
  n_adjustmentLookup = {}

  n_currentPiece = nil
  n_nextPiece = nil

  -- Add 99 frames of "entry delay" for the first piece
  for i = 1,99 do
    table.insert(n_frameQueue, "*")
  end

  startRecording()
end

function finishGame()
  if movie.active() then
    movie.stop()
  end

  -- Go to the menu
  g_menuFrameIndex = 0
  joypad.set(1, {A=true, B=false, left=false, right=false, up=false, down=false, select=false, start=false})
end




--[[-------------------------------------------------------- 
--------------------- Main Frame Loop----------------------- 
--------------------------------------------------------]]--

function eachFrame()
  if n_gameState == GameState.IN_GAME then
    runGameFrame()
  elseif n_gameState == GameState.GAME_OVER then
    -- ??
  else
    runMenuFrame()
  end
end




emu.registerafter(eachFrame)