
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

-- Where to put the fm2 files. Replace with an absolute path. to format movieName go to initSeedData
recordGames = false
moviePath = "C:\\Users\\Greg\\Desktop\\Ai"
movieName = "TestMovie"

-- Global state
gameState = 0
playstate = 0


index = 1
gameOver = false
sequence = {}
pcur = 0
pnext = 0

-- Helper function because BCD is annoying
function toDec(a)
    return 10 * (a - (a % 16)) / 16 + (a % 16);
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

--Main game loop.
while true do
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
  index = 1
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
      -- First active frame for piece. This is where board state/input sequence is calculated
      if(playstate ~= 1 or backtrack) then
          -- Check for killscreen (for my thing it would use this to top out immediately)
        local numLines = toDec(memory.readbyte(0x0051)) * 100 + toDec(memory.readbyte(0x0050))
        if numLines > 229 then
          gameOver = true
        end
        -- Gets current/next pieces before they even appear onscreen
        pcur = memory.readbyte(0x0042)
        pnext = memory.readbyte(0x0019)

        if not gameOver then
          -- local bestPlace = findMoves(orientToNum[pcur], orientToNum[pnext])
          -- Returns extra stuff here
          -- sequence, finalX, finalId = moveToSequence(placements, orientToNum[pcur])
          
        end
      end

      -- Execute input sequence
      if not gameOver then
        -- mash(index, sequence)
        index = index + 1
        if (orientToPiece[pcur] == "I" or orientToPiece[pcur] == "O") then
          joypad.set(1, {A=true,B=false,left=false,right=true,up=false,down=false,select=false,start=false})
        else
          joypad.set(1, {A=false,B=false,left=true,right=false,up=false,down=false,select=false,start=false})
        end
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
