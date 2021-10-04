import threading

from numpy import False_
import numpy
import video_capture
import socket
import time
from threading import Thread
import requests


'''
Overview:
This agent is fed game frames from the capture card, delayed by 7-8 frames.

It OCRs the first piece from the top of the board, and from then only looks at the 
next box and tracks the board state internally.

The standard event flow goes as follows:

(agent already has planned placements for the upcoming piece)
1) Wait until new piece detected at the top of the board
      (in realtime, piece has fallen 8 frames)
  a. Check the new piece in the next box
  b. Request precompute for the next placement
2) Wait 10 frames
      (in realtime, piece is either still faling, or in entry delay for next piece)
  a. Save the precompute result from the server 
'''


# -------------- Config Vars --------------
STARTING_LEVEL = 29
STARTING_LINES = 0
FIRST_PIECE_PLACEMENT = "................As."
INPUT_TIMELINE = "........X.X.X.X.X.X.X.X.X.X.X.X.X"
DELAY_FRAMES = 8

# ---------- Stateful Vars ---------------
framesToSend = None
piecesPlaced = 0
frameQueue = []
currentPiece = None
nextPiece = None
level = None
lines = None
boardLastFrame = None
framesSinceStartOfPrecompute = None
placementLookup = {}
resultStateLookup = {}
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
  HOST = '192.168.1.1'
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
          s.send(frames.encode("utf-8"))

          # Check that the raspberry pi ack's the frames
          response = s.recv(5).decode("utf-8")
          if (response != "Ack"):
              raise Exception("Request not acknowledged by the server")
        else:
          time.sleep(0.003)
          # print("nothign to send")


def hasNewlySpawnedPiece(newBoard):
  # For the very first piece, just wait till it spawns in the top row
  if piecesPlaced == 0:
    return numpy.sum(newBoard[0]) > 0

  # If there are more cells in the spawn area than before, there's a new piece
  oldTotal = 0
  newTotal = 0
  for row in range(4):
    for col in range(3,7):
      oldTotal += boardLastFrame[row][col]
      newTotal += newBoard[row][col]
  # print("New piece spawned? " + str(newTotal > oldTotal) + ". Cells in spawn zone: now= " + str(newTotal) + ", before= " + str(oldTotal))
  return newTotal > oldTotal


''' 
---------------------------
      MAIN FRAME LOOP 
---------------------------
'''


def onFrameCallback(newBoard, newNextPieceId):
  global currentPiece, nextPiece, framesSinceStartOfPrecompute, piecesPlaced, boardLastFrame

  # for row in range(10):
  #     rowStr = ""
  #     for col in range(10):
  #         rowStr += "X" if newBoard[row][col] == 1 else "."
  #     print(rowStr)

  # Detect if a new piece has spawned
  newPieceFound = hasNewlySpawnedPiece(newBoard)

  if newPieceFound and not waitingOnAsync:
    print("NEW NEXT PIECE", newNextPieceId)

    # Update current and next pieces, and look up the placement to do
    if piecesPlaced > 0:
      print("Setting current piece from", currentPiece, "to", nextPiece)
      currentPiece = nextPiece
      nextPiece = newNextPieceId
      placement = placementLookup[nextPiece]
      stateAfter = resultStateLookup[nextPiece]
    else:
      # Very first piece
      currentPiece = video_capture.identifyStartingPiece(newBoard)
      nextPiece = newNextPieceId
      [placement, stateAfter] = requestStartingPlacement(currentPiece, nextPiece)      
    
    print("Performing:", placement)
    debugLogResultBoard(stateAfter["board"])
    sendFrames(placement)
    
    # Precompute a placement for the next piece
    requestPrecompute(stateAfter)
  
  elif framesSinceStartOfPrecompute > 10 and waitingOnAsync:
    # Check for the async result
    fetchPrecomputeResult()

  # Update state for next frame
  boardLastFrame = newBoard
  framesSinceStartOfPrecompute += 1
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
    inputSequence = inputs.split("*", 1)[0][DELAY_FRAMES:]
    placementLookup[piece] = inputSequence
    resultStateLookup[piece] = {
      'board': resultBoard,
      'level': resultLevel,
      'lines': resultLines
    }
    print("Placement for piece", piece, inputSequence)

def requestPrecompute(stateAfter):
  global placementLookup, waitingOnAsync, framesSinceStartOfPrecompute, piecesPlaced
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
  framesSinceStartOfPrecompute = 0
  piecesPlaced += 1


def requestStartingPlacement(currentPiece, nextPiece):
  global piecesPlaced, placement, stateAfter
  if nextPiece == None:
    raise Exception("No next piece")
  requestStr = "http://127.0.0.1:3000/sync-nnb/{boardSerialized}/{currentPiece}/null/{level}/{lines}/0/0/0/0/0/{INPUT_TIMELINE}/true".format(
    boardSerialized = "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
    currentPiece = currentPiece,
    level = STARTING_LEVEL,
    lines = STARTING_LINES,
    INPUT_TIMELINE = INPUT_TIMELINE
  )
  print("Requesting first piece")
  response = requests.get(requestStr)
  numbers, inputs, resultBoard, resultLevel, resultLines = response.text.split("|", 5)
  inputSequence = inputs.split("*", 1)[0][DELAY_FRAMES:]

  piecesPlaced += 1
  return [inputSequence, {
    'board': resultBoard,
    'level': resultLevel,
    'lines': resultLines
  }]

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
  global piecesPlaced, frameQueue, currentPiece, nextPiece, level, lines, boardLastFrame, framesSinceStartOfPrecompute
  piecesPlaced = 0
  frameQueue = []
  currentPiece = None
  nextPiece = None
  level = STARTING_LEVEL
  lines = STARTING_LINES
  boardLastFrame = None
  framesSinceStartOfPrecompute = 0

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