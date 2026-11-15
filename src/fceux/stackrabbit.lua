local os = require("os")

-- Manual global config
USE_DAS = true -- When playing DAS, use the regular INPUT_TIMELINE to choose the level of quicktaps. 20Hz+ = 8 high quicktap, 12Hz+ = 7 high quicktap
IS_PAL = false
USE_PUSHDOWN = true
DEBUG_MODE = true
SHOULD_RECORD_GAMES = true
AGGRESSION_LEVEL = "aggro" -- How much the agent values burning lines: "safe", "aggro", or "hyper"
-- The C++ live server serving precompute (docs/engine_endpoints.md). The TS server
-- stays on 3000 until the first-move flow is ported to /precompute as well.
LIVE_SERVER_URL = "http://localhost:3000"

-- OS-dependent config
-- (We detect mac os based on common directories
IS_MAC = os.getenv("HOME") and os.getenv("HOME"):match("^/Users") ~= nil
if (IS_MAC) then
  require("rabbithttp")
else
  http = require("socket.http")
  require "socket"
end

-- NOTE: If you are on an Intel mac, please replace "rabbithttp" with "rabbithttp-intelmac" for compatibility.

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
-- The first piece's precompute (reaction time 0) is queried this many frames
-- after the kick -- much later than the usual reaction window, since there are
-- ~90 frames of startup and the result only needs to be ready before the first
-- placement starts.
FIRST_PIECE_RESULT_FRAMES = 20
INPUT_TIMELINE = TIMELINE_20_HZ;
PLAYOUT_COUNT = 160
PLAYOUT_LENGTH = 8
PRUNING_BREADTH = -2
MOVIE_PATH = "C:\\Users\\Greg\\Desktop\\VODs\\" -- Where to store the fm2 VODS (absolute path)
SCORES_TEXT_PATH = "C:\\Users\\Greg\\Desktop\\sr-test-scores.txt"
if IS_MAC then 
  MOVIE_PATH = "/Users/gregcannon/Documents/AiVods/" 
  SCORES_PATH = "/Users/gregcannon/Desktop/sr-test-scores.txt"
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
  dasCharge = 0
end
resetGameScopedVariables();

-- Reset all variables whose values are tied to one piece
function resetPieceScopedVars()
  adjustmentLookup = {}
  frameIndex = 0
  inputSequenceIndex = 0
  inputSequence = ""
  stateForNextPiece = {board=nil, level=nil, lines=nil, dasCharge=nil}
end

--[[--------------------------------------- 
------------ Helper Functions ------------- 
---------------------------------------]]--

-- Translate internal Piece IDs to actual piece types (T: 2 J: 7 Z: 9 O: 10 S: 11 L: 15 I: 18)
orientToPiece = {[0]="none", [2]="T", [7]="J", [8]="Z", [10]="O", [11]="S", [14]="L", [18]="I"}
orientToNum = {[0]="none", [2]=1, [7]=2, [8]=3, [10]=4, [11]=5, [14]=6, [18]=7}
-- Index of each piece in the C++ pipe's piece field (the I,O,L,J,T,S,Z ordering;
-- NOT the same numbering as orientToNum)
PIECE_INDEX = {I=0, O=1, L=2, J=3, T=4, S=5, Z=6}

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

-- Query into the input sequence based on (0-indexed) inputSequenceIndex
function getInputForFrame(index)
  -- We transform to uppercase since DAS strings use lowercase to visually indicate non-shift frames where buttons are held
  return string.upper(string.sub(inputSequence, index + 1, index + 1))
end

--[[------------------------------------ 
----------- HTTP Requests -------------- 
------------------------------------]]--

function parsePrecompute(precomputeResult, requestReactionTime)
  local rows = splitString(precomputeResult, "\n")

  -- Parse the initial placement and queue up those inputs. A reaction time of
  -- zero has no default placement (the piece has not been placed yet), so the
  -- row reads "Default:N/A (0 reaction time)" and must be skipped.
  if requestReactionTime > 0 then
    local defaultPlacement = splitString(rows[1], ":")[2]
    if defaultPlacement == nil then
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

  

-- Build the C++ engine's own pipe string (all 12 fields required; see
-- docs/engine_endpoints.md). nextPiece is always -1 because precompute searches
-- all seven next pieces itself; the C++ pipe parser is the input boundary and
-- rejects anything malformed. The pipe goes on the wire raw -- the HTTP clients
-- in play (libcurl / luasocket) pass query bytes verbatim and the server reads
-- the raw request line, so the URL stays readable (percent-encoding also works).
function buildPrecomputePipe(board, level, lines, pieceName, dasCharge, reactionTime)
  local pipe = board
      .. "|" .. level .. "|" .. lines
      .. "|" .. PIECE_INDEX[pieceName] .. "|-1"
      .. "|" .. INPUT_TIMELINE
      .. "|" .. PLAYOUT_COUNT .. "|" .. PLAYOUT_LENGTH .. "|" .. PRUNING_BREADTH
      .. "|" .. AGGRESSION_LEVEL .. "|" .. dasCharge .. "|" .. reactionTime .. "|"
  return pipe
end

function requestPrecompute()
  if gameOver then
    return
  end
  print("\n\nrequestPrecompute")
  -- Convert requests to PAL
  local reqLevel = tonumber(stateForNextPiece.level)
  local reqLines = tonumber(stateForNextPiece.lines)
  local reqDasCharge = tonumber(stateForNextPiece.dasCharge)
  if IS_PAL then
    reqLines = numLines + 100
    if reqLevel == 18 then
      reqLevel = 19
    elseif reqLevel == 19 then
      reqLevel = 29
    end
  end

  -- 28 Hotfix
  if reqLevel >= 27 then
    reqLevel = reqLevel - 1
    reqLines = reqLines - 10
  end

  if stateForNextPiece == nil or stateForNextPiece.board == nil
        or stateForNextPiece.lines == nil or stateForNextPiece.level == nil then
    print("GAME OVER - unknown state")
    gameOver = true
    return
  end

  local reqStr = LIVE_SERVER_URL .. "/precompute?p="
      .. buildPrecomputePipe(stateForNextPiece.board, reqLevel, reqLines,
                             orientToPiece[pnext], reqDasCharge, REACTION_TIME_FRAMES)

  local response = makeHttpRequest(reqStr)
  if response.code ~= 200 then
    error("Request not acknowledged by backend")
  end
  waitingOnAsyncRequest = true;
  return response.data
end

-- The first piece's placement: a normal precompute with reaction time 0 (the
-- piece has not been placed yet, so the result is all per-next-piece lines and
-- no default). We already know the next piece at this point, but the query is
-- the standard shape (nextPiece = -1) -- the result is indexed for the known
-- piece when it is fetched, same as every other precompute.
function requestFirstPlacement()
  local reqStr = LIVE_SERVER_URL .. "/precompute?p="
      .. buildPrecomputePipe(getEncodedBoard(), level, numLines, orientToPiece[pcur],
                             (USE_DAS and 0 or -1), 0)

  local response = makeHttpRequest(reqStr)
  if response.code ~= 200 then
    error("Request not acknowledged by backend")
  end
  waitingOnAsyncRequest = true;
  return response.data
end

-- Check if the async computation has finished, and if so make the adjustment based on it
function fetchAsyncResult()
  local response = makeHttpRequest(LIVE_SERVER_URL .. "/async-result")

  -- Only use the response if the server indicated that it sent the async result
  if response.code ~= 200 then
    error("RECEIVED BAD RESPONSE CODE:" .. response.code)
    return nil
  end 
  if (response.data == nil or response.data == '') then
    error("NO RESPONSE FROM SERVER (is it running?)")
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
  if (apiResult == nil) then
    error("apiResult was nil")
  end
  if apiResult == "none" or apiResult == "continue" then
    return
  end

  -- local split = splitString(apiResult, ",|\|")
  local split = splitString(apiResult, "\|")
  if split[3] == nil or split[4] == nil or split[5] == nil or split[6] == nil then
    error("Failed to parse game state from response")
  end
  stateForNextPiece = { 
    board=split[3], 
    level=split[4], 
    lines=split[5],
    dasCharge=split[6]
  }
  if (stateForNextPiece.dasCharge == "undefined") then 
    error("Undefined das charge")
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
  if apiResult == "none" or apiResult == nil then
    if REACTION_TIME_FRAMES == 0 then
      -- Game is over when there is no placement for a new piece
      print("GAME OVER: No adjustment")
      gameOver = true
    end
    return
  end

  -- If the backend signals us to just keep going with the default placement, then return with no change to inputSequence.
  if (isAdjustment and apiResult == "continue") then
    return
  end

  -- Parse the shifts and rotations from the API result
  -- local split = splitString(apiResult, ",|\|")
  local split = splitString(apiResult, "\|")
  inputSequence = split[2]

  -- Filter out the tildes that signify where reaction time resolves
  inputSequence = string.gsub(inputSequence, "~", "")

  if (USE_DAS and isFirstPiece) then
    -- Pre-pend a waiting frame so that DAS always starts at 0 for consistency
    inputSequence = "L.R." .. inputSequence
  end

  if inputSequence == nil or inputSequence == "none" then
    inputSequence = ""
  end

  -- Reset ARR counter if is an adjustment and can first-frame shift
  if isAdjustment then
    inputSequenceIndex = 0
  end
end

-- Compacted version of executeInputs specific to pre-arrowing on the DPAD during entry delay for DAS
function startDasDuringAre()
  local inputsThisFrame = {A=false, B=false, left=false, right=false, up=false, down=false, select=false, start=false}
  local frameStr = getInputForFrame(1) -- Not 0 since that's reserved for the _ marker for DAS during ARE
  inputsThisFrame.left = (frameStr == "L" or frameStr == "E" or frameStr == "F")
  inputsThisFrame.right = (frameStr == "R" or frameStr == "I" or frameStr == "G")
  print("during ARE " .. frameStr)
  joypad.set(1, inputsThisFrame)
  inputSequenceIndex = 1
end

function executeInputs()
  if not gameOver then

    local inputsThisFrame = {A=false, B=false, left=false, right=false, up=false, down=false, select=false, start=false}

    if inputSequence == nil or inputSequenceIndex + 1 > string.len(inputSequence) then
      -- print("Input sequence null or frame index out of bounds" .. inputSequenceIndex)
      joypad.set(1, inputsThisFrame)
      return
    end

    local thisFrameStr = getInputForFrame(inputSequenceIndex);
    
    inputsThisFrame.down = (thisFrameStr == "D")
    inputsThisFrame.A = (thisFrameStr == "A" or thisFrameStr == "E" or thisFrameStr == "I")
    inputsThisFrame.B = (thisFrameStr == "B" or thisFrameStr == "F" or thisFrameStr == "G")
    inputsThisFrame.left = (thisFrameStr == "L" or thisFrameStr == "E" or thisFrameStr == "F")
    inputsThisFrame.right = (thisFrameStr == "R" or thisFrameStr == "I" or thisFrameStr == "G")

    -- Debug logs
    -- print(thisFrameStr)

    -- Send our computed inputs to the controller
    joypad.set(1, inputsThisFrame)
  end
end


--[[------------------------------------ 
------------ Game Events  -------------- 
------------------------------------]]--

function bcdToDecimal(a)
  return 10 * (a - (a % 16)) / 16 + (a % 16)
end

function readFromMemory()
  pcur = memory.readbyte(0x42) -- Stores current/next pieces before they even appear onscreen
  pnext = memory.readbyte(0x19)
  numLines = bcdToDecimal(memory.readbyte(0x51)) * 100 + bcdToDecimal(memory.readbyte(0x50))
  level = memory.readbyte(0x44)
end

function onFirstFrameOfNewPiece()  
  readFromMemory()
    
  -- If it's the first piece, precompute the initial placement (reaction time
  -- 0; the result is fetched at FIRST_PIECE_RESULT_FRAMES)
  if isFirstPiece then
    resetPieceScopedVars()
    requestFirstPlacement()
  end

end


-- Called when the piece is locked. 
--   NOTE: THE BOARD/LEVEL/LINES ARE NOT UPDATED.  That's why there's the whole
--   shenanigans of tracking the state from the last API request
function asPieceLocks()
  print("Piece locked" .. emu.framecount() .. "  " .. frameIndex)

  -- Once the first piece locks, it's not the first piece anymore
  isFirstPiece = false

  -- Check that the real DAS charge equals the expected one for the next piece
  local dasCharge = tonumber(stateForNextPiece.dasCharge)
  if (dasCharge ~= -1 and dasCharge ~= memory.readbyte(0x46)) then
    print("Expected: " .. stateForNextPiece.dasCharge .. "Actual: " .. memory.readbyte(0x46))
    emu.pause()
    error("Mis-predicted DAS charge")
  end

  -- If it hasn't already, queue up the next precompute
  if not waitingOnAsyncRequest then
    requestPrecompute();
  end
end


-- Called when reaction time has passed and it's time to perform the adjustment
function processAdjustment()
  -- TODO: this check is a no-op in Lua (a table compares by identity against a
  -- fresh literal, so it is always false). An empty adjustmentLookup here means
  -- the precompute result never arrived -- decide whether to detect that.
  if (adjustmentLookup == {}) then
    error("No adjustment lookup found")
  end

  local adjustmentApiResult = adjustmentLookup[orientToPiece[pnext]]
  calculateInputs(adjustmentApiResult, true)
  parseGameStateFromResponse(adjustmentApiResult)

  print("Time for adjustment " .. inputSequence)
end

-- The first piece's result: fetched late (FIRST_PIECE_RESULT_FRAMES), so the
-- full precompute is ready. The response has reaction time 0 -- no default
-- placement, only per-next-piece lines -- and we already know the next piece,
-- so index straight to its line.
function processFirstAdjustment()
  local apiResult = fetchAsyncResult()
  parsePrecompute(apiResult, 0)

  local adjustmentApiResult = adjustmentLookup[orientToPiece[pnext]]
  calculateInputs(adjustmentApiResult, true)
  parseGameStateFromResponse(adjustmentApiResult)

  print("Time for adjustment " .. inputSequence)
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

  if (gameOver) then
    return
  end

  if (gamePhase == 10) then
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
  if (gamePhase == 8) then
    -- Last phase of lock delay, look up next piece stuff from server
    if (waitingOnAsyncRequest) then
      print("\n------ " .. orientToPiece[pnext] .. " ------")
      resetPieceScopedVars()
      local apiResult = fetchAsyncResult()
      parsePrecompute(apiResult, REACTION_TIME_FRAMES)
      waitingOnAsyncRequest = false
    end
    
    -- Maybe hold the Dpad for DAS
    if (USE_DAS and getInputForFrame(0) == "_") then
      startDasDuringAre()
    end

  elseif(gamePhase == 1) then
    if(gamePhaseLastFrame ~= 1) then
      -- First active frame for piece. This is where board state/input sequence is calculated
      onFirstFrameOfNewPiece()
    end
    if isFirstPiece then
      -- The first piece: fetch its precompute (reaction time 0) after a long
      -- buffer -- there are ~90 startup frames, so starting the placement at
      -- FIRST_PIECE_RESULT_FRAMES is comfortable.
      if frameIndex == FIRST_PIECE_RESULT_FRAMES then
        processFirstAdjustment()
      end
    elseif frameIndex == REACTION_TIME_FRAMES and SHOULD_ADJUST then
      -- Once reaction time is over, handle adjustment     
      processAdjustment()
    elseif frameIndex == REACTION_TIME_FRAMES + 1 then
      -- Precompute the next placement
      requestPrecompute()
    end

    -- Execute input sequence
    executeInputs()
    frameIndex = frameIndex + 1
    inputSequenceIndex = inputSequenceIndex + 1

  -- Do stuff right when the piece locks.
  elseif gamePhase >= 2 and gamePhase < 8 then
    if gamePhaseLastFrame == 1 then
      if not USE_PUSHDOWN and not isFirstPiece and not gameOver and getInputForFrame(inputSequenceIndex + 1) ~= "*" then
        if (DEBUG_MODE) then
          error("Server mistimed lock delay")
        end
      end
      asPieceLocks()
      return
    end

    -- If the agent is mistaken about the board state, crash immediately so I can debug it
    if gamePhase == 6 and not gameOver and getEncodedBoard() ~= stateForNextPiece.board then
      if (DEBUG_MODE) then
        emu.pause()
        print(getEncodedBoard())
        print(stateForNextPiece.board)
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
