IS_MAC = false
IS_PAL = false
USE_PUSHDOWN = true
DEBUG_MODE = false

local os = require("os")
if (IS_MAC) then
  require("rabbithttp")
else
  http = require("socket.http")
  require "socket"
end

TIMELINE_2_HZ = "X.............................";
TIMELINE_6_HZ = "X........";
TIMELINE_7_HZ = "X.......";
TIMELINE_8_HZ = "X......";
TIMELINE_10_HZ = "X.....";
TIMELINE_11_HZ = "X.....X....X....";
TIMELINE_12_HZ = "X....";
TIMELINE_13_HZ = "X....X...";
TIMELINE_13_5_HZ = "X....X...X...";
TIMELINE_14_HZ = "X....X...X...X...";
TIMELINE_15_HZ = "X...";
TIMELINE_20_HZ = "X..";
TIMELINE_30_HZ = "X.";

-- Config constants
SHOULD_ADJUST = true
REACTION_TIME_FRAMES = 18
INPUT_TIMELINE = TIMELINE_30_HZ;
SHOULD_RECORD_GAMES = false
MOVIE_PATH = "C:\\Users\\Greg\\Desktop\\VODs\\" -- Where to store the fm2 VODS (absolute path)
SCORES_TEXT_PATH = "C:\\Users\\Greg\\Desktop\\sr-test-scores.txt"
if IS_MAC then 
  MOVIE_PATH = "/Users/greg/Documents/AiVods/" 
  SCORES_PATH = "/Users/greg/Desktop/sr-test-scores.txt"
end
-- file = io.open(SCORES_PATH, "StackRabbit Game Scores:")


function resetGameScopedVariables()
  isFirstPiece = true
  firstPieceDelayFrames = 10
  metaGameState = 0
  gamePhase = 0
  numLines = 0
  waitingOnAsyncRequest = false
  gameOver = false
  pcur = 0
  pnext = 0
end
resetGameScopedVariables();

-- Reset all variables whose values are tied to one piece
function resetPieceScopedVars()
  adjustmentLookup = {}
  frameIndex = 0
  arrFrameIndex = 0
  inputSequence = ""
  shiftsExecuted = 0
  rotationsExecuted = 0
  stateForNextPiece = {board=nil, level=nil, lines=nil}
end

--[[--------------------------------------- 
------------ Helper Functions ------------- 
---------------------------------------]]--

-- Translate internal Piece IDs to actual piece types (T: 2 J: 7 Z: 9 O: 10 S: 11 L: 15 I: 18)
orientToPiece = {[0]="none", [2]="T", [7]="J", [8]="Z", [10]="O", [11]="S", [14]="L", [18]="I"}
orientToNum = {[0]="none", [2]=1, [7]=2, [8]=3, [10]=4, [11]=5, [14]=6, [18]=7}

function startRecording()
  if(SHOULD_RECORD_GAMES) then
    local dateStr = os.date("%m-%d %H %M")
    print(dateStr)
    movieName = "StackRabbit" .. dateStr
    print(movieName)
    movie.record(MOVIE_PATH .. movieName .. ".fm2", 1)
  end
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

-- Query into the input sequence based on (0-indexed) arrFrameIndex
function getInputForFrame(index)
  return string.sub(inputSequence, index + 1, index + 1)
end

--[[------------------------------------ 
----------- HTTP Requests -------------- 
------------------------------------]]--

function parsePrecompute(precomputeResult)
  local rows = splitString(precomputeResult, "\n")

  -- Parse the initial placement and queue up those inputs
  if REACTION_TIME_FRAMES > 0 then
    local defaultPlacement = splitString(rows[1], ":")[2]
    if defaultPlacement == null then
      print("GAME OVER - no default placement")
      gameOver = true
      return
    end
    print("Initial placement: " .. defaultPlacement)
    calculateInputs(defaultPlacement, false)
    parseGameStateFromResponse(defaultPlacement)
  end

  -- Store all the adjustments in a lookup table
  for i = 2,8 do
    local resultSplit = splitString(rows[i], ":")
    local pieceStr = resultSplit[1]
    adjustmentLookup[pieceStr] = resultSplit[2]
  end
end

  

-- Make a request that will kick off a longer calculation. Subsequent frames will ping the server again for the result.
function requestAdjustmentAsync()
  offsetXAtAdjustmentTime = 0
  rotationAtAdjustmentTime = 0
  canFirstFrameShiftAtAdjustmentTime = true
  offsetYAtAdjustmentTime = 0
  -- Convert requests to PAL
  local reqLevel = level
  local reqLines = numLines
  print("reqLines " .. reqLines)
  if IS_PAL then
    reqLines = numLines + 100
    if reqLevel == 18 then
      reqLevel = 19
    elseif reqLevel == 19 then
      reqLevel = 29
    end
  end
  print("reqLines2 " .. reqLines)

  -- Format URL arguments
  local reqStr = "http://localhost:3000/get-move-async?board=" .. getEncodedBoard() .. "&currentPiece=" .. orientToPiece[pcur]
  reqStr = reqStr .. "&nextPiece=" .. orientToPiece[pnext] .. "&level=" .. reqLevel .. "&lines=" .. reqLines .. "&inputFrameTimeline=" .. INPUT_TIMELINE

  local response = makeHttpRequest(reqStr)
  if response.code ~= 200 then
    error("Request not acknowledged by backend")
  end
  waitingOnAsyncRequest = true
  return response.data
end

function requestPrecompute()
  if gameOver then
    return
  end
  print("requestprecompute")
  -- Convert requests to PAL
  local reqLevel = stateForNextPiece.level
  local reqLines = stateForNextPiece.lines
  print("reqLines " .. reqLines)
  if IS_PAL then
    reqLines = numLines + 100
    if reqLevel == 18 then
      reqLevel = 19
    elseif reqLevel == 19 then
      reqLevel = 29
    end
  end
  print("reqLines2 " .. reqLines)

  -- Format URL arguments
  if stateForNextPiece == nil or stateForNextPiece.board == nil
        or stateForNextPiece.lines == nil or stateForNextPiece.level == nil then
    print("GAME OVER - unknown state")
    gameOver = true
    return
  end

  local reqStr = "http://localhost:3000/precompute?board=" .. stateForNextPiece.board .. "&currentPiece=" .. orientToPiece[pnext]
  reqStr = reqStr .. "&level=" .. stateForNextPiece.level .. "&lines=" .. reqLines .. "&reactionTime="
  reqStr = reqStr .. REACTION_TIME_FRAMES .. "&inputFrameTimeline=" .. INPUT_TIMELINE

  local response = makeHttpRequest(reqStr)
  if response.code ~= 200 then
    error("Request not acknowledged by backend")
  end
  waitingOnAsyncRequest = true; 
  return response.data
end

-- Check if the async computation has finished, and if so make the adjustment based on it
function fetchAsyncResult()
  local response = makeHttpRequest("http://localhost:3000/async-result")

  -- Only use the response if the server indicated that it sent the async result
  if response.code ~= 200 then
    error("RECEIVED BAD RESPONSE CODE:" .. response.code)
    return nil
  end 
  waitingOnAsyncRequest = false;
  return response.data
end

function makeHttpRequest(requestUrl)
  print(requestUrl)

  -- On Mac, use custom C library for HTTP requests
  if IS_MAC then
    return {data= httpFetch(requestUrl), code=200}
  end

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

function parseGameStateFromResponse(apiResult)
  if apiResult == "No legal moves" or apiResult == nil then
    return
  end

  -- local split = splitString(apiResult, ",|\|")
  local split = splitString(apiResult, "\|")
  
  if split[3] ~= nil and split[4] ~= nil and split[5] ~= nil then
    stateForNextPiece = { 
      board=split[3], 
      level=split[4], 
      lines=split[5] 
    }
  end
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
  if apiResult == "No legal moves" or apiResult == nil then
    if REACTION_TIME_FRAMES == 0 then
      -- Game is over when there is no placement for a new piece
      print("GAME OVER: No adjustment")
      gameOver = true
    end
    return
  end

  -- Parse the shifts and rotations from the API result
  -- local split = splitString(apiResult, ",|\|")
  local split = splitString(apiResult, "\|")
  inputSequence = split[2]
  if inputSequence == nil or inputSequence == "none" then
    inputSequence = ""
  end

  -- Reset ARR counter if is an adjustment and can first-frame shift
  if isAdjustment then
    arrFrameIndex = 0
  end
end


function executeInputs()
  if not gameOver then

    local inputsThisFrame = {A=false, B=false, left=false, right=false, up=false, down=false, select=false, start=false}

    if inputSequence == null or arrFrameIndex + 1 > string.len(inputSequence) then
      -- print("Input sequence null or frame index out of bounds" .. arrFrameIndex)
      -- print(inputSequence)
      joypad.set(1, inputsThisFrame)
      return
    end

    local thisFrameStr = getInputForFrame(arrFrameIndex);
    -- print(arrFrameIndex .. "  " .. thisFrameStr)
    
    inputsThisFrame.down = (thisFrameStr == "D")
    inputsThisFrame.A = (thisFrameStr == "A" or thisFrameStr == "E" or thisFrameStr == "I")
    inputsThisFrame.B = (thisFrameStr == "B" or thisFrameStr == "F" or thisFrameStr == "G")
    inputsThisFrame.left = (thisFrameStr == "L" or thisFrameStr == "E" or thisFrameStr == "F")
    inputsThisFrame.right = (thisFrameStr == "R" or thisFrameStr == "I" or thisFrameStr == "G")

    if inputsThisFrame.left then
      shiftsExecuted = shiftsExecuted - 1
    elseif inputsThisFrame.right then
      shiftsExecuted = shiftsExecuted + 1
    end

    -- Debug logs
    if inputsThisFrame.left or inputsThisFrame.right then
      print("SHIFT" .. emu.framecount())
    end

    -- Send our computed inputs to the controller
    joypad.set(1, inputsThisFrame)
  end
end


--[[------------------------------------ 
------------ Game Events  -------------- 
------------------------------------]]--

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

  -- If it's the first piece, make an 'adjustment' to do the initial placement
  if isFirstPiece then
    requestAdjustmentAsync()
  
  elseif not gameOver and waitingOnAsyncRequest then
    -- Check in on the result of the previous async request for the inital placement
    local apiResult = fetchAsyncResult()
    parsePrecompute(apiResult)
  end
end


-- Called when the piece is locked. 
--   NOTE: THE BOARD/LEVEL/LINES ARE NOT UPDATED.  That's why there's the whole
--   shenanigans of tracking the state from the last API request
function asPieceLocks()
  print("Piece locked" .. emu.framecount() .. "  " .. frameIndex)

  -- Once the first piece locks, it's not the first piece anymore
  isFirstPiece = false

  -- If it hasn't already, queue up the next precompute
  if not waitingOnAsyncRequest then
    requestPrecompute();
  end
end


-- Called when reaction time has passed and it's time to perform the adjustment
function processAdjustment()
  if (adjustmentLookup == {}) then
    error("No adjustment lookup found")
  end
  print("Time for adjustment " .. frameIndex .. ", " .. arrFrameIndex)

  if isFirstPiece then
    local adjustmentApiResult = fetchAsyncResult()
    calculateInputs(adjustmentApiResult, true)
    parseGameStateFromResponse(adjustmentApiResult)
  else 
    local adjustmentApiResult = adjustmentLookup[orientToPiece[pnext]]
    calculateInputs(adjustmentApiResult, true)
    parseGameStateFromResponse(adjustmentApiResult)
  end 
end

function onGameOver()
  -- file:write("hello", "\n")
  -- file:write("hello", "\n")
end

--[[------------------------------------ 
---------- Main Game Loop  ------------- 
------------------------------------]]--

function runGameFrame()
  -- To account for TetrisGYM issues, ignore the first few frames of a new game start
  if isFirstPiece and firstPieceDelayFrames > 0 then
    firstPieceDelayFrames = firstPieceDelayFrames - 1
    print("Skipping frame" .. firstPieceDelayFrames)
    return
  end

  if gamePhase == 10 then
    -- Quit to menu
    startBtnVal = false;
    if emu.framecount() % 10 == 1 then
      startBtnVal = true
    end
    joypad.set(1, {A=false,B=false,left=false,right=false,up=false,down=false,select=false,start=startBtnVal});
    return
  end

  local gamePhaseLastFrame = gamePhase
  gamePhase = memory.readbyte(0x0048)
  -- print("gamePhase" .. gamePhase)
  if(gamePhase == 1) then
    if(gamePhaseLastFrame ~= 1) then
      -- First active frame for piece. This is where board state/input sequence is calculated
      onFirstFrameOfNewPiece()
    end
    if frameIndex == REACTION_TIME_FRAMES and (SHOULD_ADJUST or isFirstPiece)  then
      -- Once reaction time is over, handle adjustment     
      processAdjustment()
    elseif frameIndex == REACTION_TIME_FRAMES + 1 then
      -- Precompute the next placement
      requestPrecompute()
    end

    -- Execute input sequence
    executeInputs()
    frameIndex = frameIndex + 1
    arrFrameIndex = arrFrameIndex + 1

  -- Do stuff right when the piece locks.
  elseif gamePhase >= 2 and gamePhase <= 8 then
    if gamePhaseLastFrame == 1 then
      if not USE_PUSHDOWN and not isFirstPiece and not gameOver and getInputForFrame(arrFrameIndex + 1) ~= "*" then
        print(inputSequence)
        if (DEBUG_MODE) then
          error("Server mistimed lock delay")
        end
      end
      asPieceLocks()
      return
    end
    -- If the agent is mistaken about the board state, crash immediately so I can debug it
    if gamePhase == 8 and not gameOver and getEncodedBoard() ~= stateForNextPiece.board then
      if (DEBUG_MODE) then
        error("Divergence")
      end
    end

  -- Resets the index for the next piece. Disables user input when the game is not over.
  elseif not gameOver or recording then
    joypad.set(1, {A=false,B=false,left=false,right=false,up=false,down=false,select=false,start=false})
  end
end




--[[-------------------------------------------------------- 
--------------------- Main Frame Loop----------------------- 
--------------------------------------------------------]]--


function eachFrame()
  --Update metaGameState
  local metaGameStateLastFrame = metaGameState
  metaGameState = memory.readbyte(0x00C0)
  -- print("Metastate" .. metaGameState)

  --Game starts
  if(metaGameStateLastFrame == 3 and metaGameState == 4) then
    startRecording()
  end

  --Game ends, clean up data
  if(metaGameStateLastFrame == 4 and metaGameState == 3) then
  resetGameScopedVariables()
  if movie.active() then
      movie.stop()
      end
  end

  if(metaGameState < 4) then
    -- Currently on menu
    startBtnVal = false
    aBtnVal = false
    if emu.framecount() % 10 == 4 then
      startBtnVal = true
    end
    if emu.framecount() % 10 < 5 then
      aBtnVal = true
    end
    joypad.set(1, {A=aBtnVal,B=false,left=false,right=false,up=false,down=false,select=false,start=startBtnVal});
    return
  end

  if(metaGameState == 4) then
    runGameFrame()
  end
end

emu.registerafter(eachFrame)
