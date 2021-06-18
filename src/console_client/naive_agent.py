import threading

from numpy import False_
import video_capture
import socket
import time
from threading import Thread
import requests

# -------------- Config Vars --------------
STARTING_LEVEL = 19
FIRST_PIECE_PLACEMENT = "..........As......."
INPUT_TIMELINE = "........X.X.X.X.X.X.X.X.X.X.X.X.X"
DELAY_FRAMES = 8
MAX_INPUTS = 15

# ---------- Stateful Vars ---------------
framesToSend = None
piecesPlaced = 0
frameQueue = []
currentPiece = None
nextPiece = None
level = STARTING_LEVEL
lines = 0
boardLastFrame = None
framesSinceLastPlacement = None

placementLookup = {
  "T": "A.A",
  "I": "L.L.L",
  "S": "R.R.R",
  "Z": "I.R.R",
  "O": "L.L.L.L",
  "L": "F.L.L.L.L",
  "J": "E.E.L.L",
}
resultStateLookup = {
  'T': {
    'board': "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000100000000111000",
    'level': STARTING_LEVEL,
    'lines': 0
  },
  'I': {
    'board': "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000001111000000",
    'level': STARTING_LEVEL,
    'lines': 0
  },
  'S': {
    'board': "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000110000000110",
    'level': STARTING_LEVEL,
    'lines': 0
  },
  'Z': {
    'board': "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000100000000110000000010",
    'level': STARTING_LEVEL,
    'lines': 0
  },
  'O': {
    'board': "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000011000000001100000000",
    'level': STARTING_LEVEL,
    'lines': 0
  },
  'L': {
    'board':"00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000100000000010000000001100000000",
    'level': STARTING_LEVEL,
    'lines': 0
  },
  'J': {
    'board': "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000010000000001110000000",
    'level': STARTING_LEVEL,
    'lines': 0
  },
}
waitingOnAsync = False

EMPTY_BOARD = []
for r in range(20):
  EMPTY_BOARD.append([0,0,0,0,0,0,0,0,0,0])

def sendFrames(frames):
  global framesToSend
  if framesToSend != None:
    raise Exception("Already sending frames!")
  framesToSend = frames

def socketThread():
  global framesToSend
  # HOST = '127.0.0.1'  # The server's hostname or IP address
  HOST = '192.168.1.5'
  PORT = 6000        # The port used by the server

  with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
      print("Starting socket connection")
      s.connect((HOST, PORT))
      print("Connected to socket")
      
      while True:
        if framesToSend != None:
          # Send the frames to the raspberry pi
          frames = framesToSend
          framesToSend = None
          stt = time.time()
          s.send(frames.encode("utf-8"))

          # Check that the raspberry pi ack's the frames
          response = s.recv(5).decode("utf-8")
          if (response != "Ack"):
              raise Exception("Request not acknowledged by the server")
        else:
          time.sleep(0.003)
          # print("nothign to send")


def hasNewlySpawnedPiece(newBoard):
  # Only possible if it's been a while since the last placement
  framesToWaitSinceLastNewPiece = 12 
  if piecesPlaced == 0 and level < 28:
    framesToWaitSinceLastNewPiece = 12
  elif piecesPlaced == 0:
    framesToWaitSinceLastNewPiece = 20
  elif level >= 29:
    framesToWaitSinceLastNewPiece = 8
  else:
    framesToWaitSinceLastNewPiece = 8
  if framesSinceLastPlacement < framesToWaitSinceLastNewPiece:
    print("Not ready")
    return False

  if piecesPlaced == 0:
    print("Automatically new for first piece")
    return True

  # If any cells have changed at the top of the board, we have a new piece
  for row in range(4):
    for col in range(3,7):
      if newBoard[row][col] != boardLastFrame[row][col]:
        return True
  return False


''' 
---------------------------
      MAIN FRAME LOOP 
---------------------------
'''


def onFrameCallback(newBoard, newNextPieceId):
  global currentPiece, nextPiece, framesSinceLastPlacement, piecesPlaced, boardLastFrame

  # for row in range(10):
  #     rowStr = ""
  #     for col in range(10):
  #         rowStr += "X" if newBoard[row][col] == 1 else "."
  #     print(rowStr)

  # Detect if a new piece has spawned
  newPieceFound = hasNewlySpawnedPiece(newBoard)
  print("onFrame", currentPiece, nextPiece, newNextPieceId, framesSinceLastPlacement, "newpiece:", newPieceFound, "placed", piecesPlaced)

  if newPieceFound and not waitingOnAsync:
    print("NEW NEXT PIECE", newNextPieceId)

    # Update current and next pieces
    if piecesPlaced > 0:
      print("Setting current piece from", currentPiece, "to", nextPiece)
      currentPiece = nextPiece
      nextPiece = newNextPieceId
    else:
      # Very first piece
      currentPiece = video_capture.identifyStartingPiece(newBoard)
      nextPiece = newNextPieceId
      print("FOUND CURRENT PIECE:", currentPiece)

    # Look up the placement for the current piece
    if piecesPlaced < 1:
      placement = placementLookup[currentPiece]
      stateAfter = resultStateLookup[currentPiece]
    else:
      placement = placementLookup[nextPiece]
      stateAfter = resultStateLookup[nextPiece]
    
    print("Performing:", placement)
    debugLogResultBoard(stateAfter["board"])
    sendFrames(placement)
    
    # Precompute a placement for the next piece
    requestPrecompute(stateAfter)
    framesSinceLastPlacement = 0
    piecesPlaced += 1
  
  elif framesSinceLastPlacement > (15 if level < 29 else 7) and waitingOnAsync:
    # Check for the async result
    fetchPrecomputeResult()

  # Update state for next frame
  boardLastFrame = newBoard
  framesSinceLastPlacement += 1
  pass

'''
Clear out a floating piece if there is one.
    (The piece will always be in cols 3-6.)
'''
def clearFloatingPiece(board):
  clearedBoard = board.copy()
  startedClearing = False
  for row in range(19,-1, -1):
      if startedClearing:
          for col in range(10):
              clearedBoard[row][col] = 0
      else:
          rowEmpty = True
          for col in range(10):
              if clearedBoard[row][col] == 1:
                  rowEmpty = False
                  break
          if rowEmpty:
              startedClearing = True
  return clearedBoard

# def serializeBoard():
#   if boardLastFrame == None:
#     return EMPTY_BOARD
#   clearedBoard = clearFloatingPiece(boardLastFrame)
#   # print("Serializing...")
#   # print(boardLastFrame)
#   boardStr = ""
#   for r in range(20):
#     for c in range(10):
#       boardStr += "1" if clearedBoard[r][c] else "0"
#   return boardStr

def debugLogResultBoard(resultBoard):
  for row in range(20):
      rowStr = ""
      for col in range(10):
          rowStr += "X" if resultBoard[row * 10 + col] == "1" else "."
      print(rowStr)

def fetchPrecomputeResult():
  global placementLookup, resultStateLookup, waitingOnAsync
  response = requests.get("http://127.0.0.1:3000/async-result")
  if response.status_code != 200:
    print("Server not ready yet")
    return
  
  # Parse and save the result
  waitingOnAsync = False
  responseLines = response.text.split("\n")
  placementLookup = {}
  resultStateLookup = {}
  for i in range(1,8):
    piece, rest = responseLines[i].split(":")
    numbers, inputs, resultBoard, resultLevel, resultLines = rest.split("|", 5)
    inputSequence = inputs.split("*", 1)[0][DELAY_FRAMES:DELAY_FRAMES + MAX_INPUTS]
    placementLookup[piece] = inputSequence
    resultStateLookup[piece] = {
      'board': resultBoard,
      'level': resultLevel,
      'lines': resultLines
    }
    print("Placement for piece", piece, inputSequence)

def requestPrecompute(stateAfter):
  global placementLookup, waitingOnAsync
  if nextPiece == None:
    raise Exception("No next piece")
  placementLookup = None
  requestStr = "http://127.0.0.1:3000/precompute/{boardSerialized}/{nextPiece}/null/{level}/{lines}/0/0/0/0/0/{INPUT_TIMELINE}/true".format(
    boardSerialized = stateAfter["board"],
    nextPiece = nextPiece,
    level = stateAfter["level"],
    lines = stateAfter["lines"],
    INPUT_TIMELINE = INPUT_TIMELINE
  )
  print("Requesting precompute")
  response = requests.get(requestStr)
  print("Initial ack:", response.text)
  waitingOnAsync = True


# def startTestCapture(onFrameCallback):
#   testPieces = ['T', 'L', 'I', 'O', 'J', 'S', 'Z', 'T', 'L', 'I', 'O', 'J', 'S', 'Z', 'T', 'L', 'I', 'O', 'J', 'S', 'Z']
#   countdown = 10
#   lastSeen = None

#   while len(testPieces) > 0:
#     # Maybe update the last seen piece
#     if countdown == 0:
#       lastSeen = testPieces.pop()
#       countdown = 10

#     # Always provide a capture of its most recently seen state    
#     if lastSeen == None:
#       onFrameCallback(EMPTY_BOARD, "-")
#     else:
#       onFrameCallback(EMPTY_BOARD, lastSeen)
#     countdown -= 1
#     time.sleep(0.03333333)
    

def start():
  global piecesPlaced, frameQueue, currentPiece, nextPiece, level, lines, boardLastFrame, framesSinceLastPlacement
  piecesPlaced = 0
  frameQueue = []
  currentPiece = None
  nextPiece = None
  level = STARTING_LEVEL
  boardLastFrame = None
  framesSinceLastPlacement = 0

  # Start the capture and the socket thread
  socketWorker = Thread(target=socketThread)
  socketWorker.daemon = True
  socketWorker.start()

  sendFrames(FIRST_PIECE_PLACEMENT)
  video_capture.startCapture(onFrameCallback)

  # startTestCapture(onFrameCallback)


# socketWorker = Thread(target=socketThread)
# socketWorker.daemon = True
# # socketWorker.start()
# for i in range(10):
#   st = time.time()
#   # sendFrames("A")
#   kk = requests.get("http://127.0.0.1:3000/ping")
#   print(time.time() - st)
#   time.sleep(1)
start()