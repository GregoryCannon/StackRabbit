-- If you created readRanks.lua and put api calls here then it would function the same way
-- readRanks = require "readRanks"


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
-- Cutoff for when to stop trying to score tetrises
threshold = 229

-- Obsolete with API calls
rankPath = "/home/justin/proj/tetris/acervus/data/ranksEv9.1"

-- Where to put the fm2 files. Replace with an absolute path. to format movieName go to initSeedData
recordGames = false
moviePath = ""
movieName = ""
-- readRanks.openRankFile(rankPath) -- 

-- Global state
gameState = 0
playstate = 0
seed = 0
pc = 0
offset = 0


index = 1
gameOver = false
sequence = {}
pcur = 0
pnext = 0

-- Helper function because BCD is annoying
function toDec(a)
	return 10 * (a - (a % 16)) / 16 + (a % 16);
end

function getRank(surface)
  -- return readRanks.getRank(surface)
  return 3
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

--The main function, gives the best placement
function findMoves(cur, next)
  local board = getBoard()
  print(board);
  local placements = getValidMoves(columns, cur)
  -- bestPlace is probably a structrue or smth
  -- local bestPlace = something

  -- Loop through all valid moves
  -- for i,place in ipairs(placements) do
  -- 	-- Make placement, get value with API call, usual stuff :P
  -- end

  return bestPlace
end

-- Returns a list of all considered moves (here it's anything that doesn't create a hole)
function getValidMoves(cols, piece)
  -- Do stuff here
  
  return placements
end

-- Does its namesake, but only for standard right well
function tetrisReady(board)
  for i=17,20 do
    for j=1,9 do
      if(board[i][j] == 239) then
        return false
      end
    end
  end
  return true
end

-- Oh god this is awful. Hopefully you'll translate this to how you handle it.
function moveToSequence(piece, pos, orient)
  sequence = {}
	-- Each entry of sequence can be initialized by input = {A=false,B=false,left=false,right=false,up=false,down=false,select=false,start=false}
	-- This assumes the entire input sequence is planned beforehand, so basically fill the sequence with a bunch of copies of the above,
	-- and then change certain entries (e.g. input.left = true)
	-- Originally this also returned the expected final x position/rotation of the piece center. The latter can be found with getId[piece][orient] 
end

-- Executes the input sequence supplied by the aptly named variable "sequence"
-- index is a global variable that increments with each frame, and sequence is the exact input sequence calculated
-- if you have your first input when index is 1, it will execute on the first frame
function mash(index, sequence)
  if sequence[index] ~= nil then
    joypad.set(1, sequence[index])
  else
  	-- This has the effect of disabling any user input in the middle of a game, which is convenient
    joypad.set(1, {A=false,B=false,left=false,right=false,up=false,down=false,select=false,start=false})
  end
end

 -- If you want to add overlay stuff put it here
function hud()
  gui.text(8, 9, string.format("SEED: %04X", seed))
  gui.text(8, 17, "PCNT: " .. pc)
  gui.text(8, 25, "OFFS: ".. offset)
end

-- Gets the starting seed. This executes right when the game realizes start is pressed so it's very accurate. Nothing else really necessitates this level of precision.
function initSeedData()
  seed = memory.readword(0x0018, 0x0017)
  pc = (memory.readbyte(0x001A)) % 8
  offset = (memory.readbyte(0x00B1)) % 4
  movieName = string.format("%04X-%d-%d", seed, pc, offset)
end

memory.registerexec(0x84AE, initSeedData)
gui.register(hud)

--Main game loop.
while true do
	--Game starts
	if(gameState == 3 and memory.readbyte(0x00C0) == 4) then
		if(recordGames) then
    	movie.record(moviePath .. movieName .. ".fm2", 1, "fractal161")
    end
	end

	--Check if a reset, hard reset, save state(?) has been loaded.
	if(gameState == 4 and memory.readbyte(0x00C0) < 3) then
    -- Panic
	end

	--Game ends, clean up data
	if(gameState == 4 and memory.readbyte(0x00C0) == 3) then
    gameOver = false
    index = 1
    if movie.active() then
    	movie.stop()
		end
    movieName = ""
	end

	--Update gameState
	gameState = memory.readbyte(0x00C0)

	--Update menu stats
  if(gameState < 4) then
    seed = memory.readword(0x0018, 0x0017)
    pc = (memory.readbyte(0x001A)) % 8
    offset = (memory.readbyte(0x00B1)) % 4
  end

	if(gameState == 4) then
    if(memory.readbyte(0x0048) == 1) then
      -- First active frame for piece. This is where board state/input sequence is calculated
      if(playstate ~= 1 or backtrack) then
      	-- Check for killscreen (for my thing it would use this to top out immediately)
        local numLines = toDec(memory.readbyte(0x0051)) * 100 + toDec(memory.readbyte(0x0050))
        if numLines > threshold then
          gameOver = true
        end
        -- Gets current/next pieces before they even appear onscreen
        pcur = memory.readbyte(0x0042)
        pnext = memory.readbyte(0x0019)

        if not gameOver then
          local bestPlace = findMoves(orientToNum[pcur], orientToNum[pnext])
          -- Returns extra stuff here
          sequence, finalX, finalId = moveToSequence(placements, orientToNum[pcur])
        end
      end
      -- Execute input sequence
      if not gameOver then
        mash(index, sequence)
        index = index + 1
      end
    -- Do stuff right when the piece locks. If you want to check that the piece went to the correct spot/send an API request early here is probably good.
    elseif(memory.readbyte(0x0048) == 2 and playstate == 1) then     
    -- Detects when the game is over.
    elseif memory.readbyte(0x0048) == 10 then
    	gameOver = true
		-- Resets the index for the next piece. Disables user input when the game is not over.
    elseif not gameOver or recording then
      index = 1
      joypad.set(1, {A=false,B=false,left=false,right=false,up=false,down=false,select=false,start=false})
    end
	end

  playstate = memory.readbyte(0x0048)
  emu.frameadvance()
end
