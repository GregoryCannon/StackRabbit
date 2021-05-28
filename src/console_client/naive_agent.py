import threading

from numpy import False_
import video_capture
import socket
import time
from threading import Thread
import requests

# -------------- Config Vars --------------
STARTING_LEVEL = 18
FIRST_PIECE_PLACEMENT = "A.A"
INPUT_TIMELINE = "X."

# ---------- Stateful Vars ---------------
framesToSend = None
isFirstPiece = True
frameQueue = []
currentPiece = None
nextPiece = None
level = STARTING_LEVEL
lines = 0
currentBoard = None
framesSinceLastPlacement = None
placementLookup = None # For storing the API result
waitingOnAsync = False

EMPTY_BOARD = []
for r in range(20):
  EMPTY_BOARD.append([0,0,0,0,0,0,0,0,0,0])

def sendFrames(frames):
  global framesToSend
  print("SendFrames called")
  if framesToSend != None:
    raise Exception("Already sending frames!")
  framesToSend = frames

def socketThread():
  global framesToSend
  HOST = '127.0.0.1'  # The server's hostname or IP address
  # HOST = '192.168.1.5'
  PORT = 6000        # The port used by the server

  with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
      print("Starting socket connection")
      s.connect((HOST, PORT))
      print("Connected to socket")
      
      while True:
        if framesToSend != None:
          # Send the frames to the raspberry pi
          frames = framesToSend
          print("Queue is empty")
          framesToSend = None
          stt = time.time()
          s.send(frames.encode("utf-8"))

          # Check that the raspberry pi ack's the frames
          response = s.recv(5).decode("utf-8")
          print("Socket roundtrip took", time.time() - stt)
          print("Got response", response)
          if (response != "Ack"):
              raise Exception("Request not acknowledged by the server")
        else:
          time.sleep(0.002)
          # print("nothign to send")


def hasNewlySpawnedPiece(newBoard):
  global currentBoard

  # Only possible if it's been a while since the last placement
  if framesSinceLastPlacement >= 38:
    return True
    # If any cells have changed at the top of the board, we have a new piece
    for row in range(4):
      for col in range(3,7):
        if newBoard[row][col] != currentBoard[row][col]:
          return True
  return False

def onFrameCallback(newBoard, newNextPieceId):
  global currentPiece, nextPiece, framesSinceLastPlacement, isFirstPiece, currentBoard
  print("onFrame", newNextPieceId, framesSinceLastPlacement)

  # Detect if a new piece has spawned
  if hasNewlySpawnedPiece(newBoard) and not waitingOnAsync:
    print("NEW NEXT PIECE", newNextPieceId)

    # Look up the placement for the current piece
    if not isFirstPiece:
      placement = placementLookup[newNextPieceId]
      print("Performing:", placement)
      sendFrames(placement)

    # Precompute a placement for the next piece
    isFirstPiece = False
    currentPiece = nextPiece
    nextPiece = newNextPieceId
    currentBoard = newBoard
    requestPrecompute()
    framesSinceLastPlacement = 0
  
  elif framesSinceLastPlacement > 15 and waitingOnAsync:
    # Check for the async result
    fetchPrecomputeResult()

  framesSinceLastPlacement += 1
  pass


def serializeBoard():
  if currentBoard == None:
    return EMPTY_BOARD

  # print("Serializing...")
  # print(currentBoard)
  boardStr = ""
  for r in range(20):
    for c in range(10):
      boardStr += "1" if currentBoard[r][c] else "0"
  return boardStr

def fetchPrecomputeResult():
  global placementLookup, waitingOnAsync
  response = requests.get("http://127.0.0.1:8080/async-result")
  if response.status_code != 200:
    print("Server not ready yet")
    return
  
  # Parse and save the result
  waitingOnAsync = False
  responseLines = response.text.split("\n")
  placementLookup = {}
  for i in range(1,8):
    piece, rest = responseLines[i].split(":")
    numbers, rest2 = rest.split("|", 1)
    inputSequence = rest2.split("*", 1)[0][0:15]
    placementLookup[piece] = inputSequence
    print("Placement for piece", piece, inputSequence)


def requestPrecompute():
  global placementLookup, waitingOnAsync
  if nextPiece == None:
    raise Exception("No next piece")
  placementLookup = None
  requestStr = "http://127.0.0.1:8080/precompute-naive/{boardSerialized}/{nextPiece}/null/{level}/{lines}/0/0/0/10/{INPUT_TIMELINE}/true".format(
    boardSerialized = serializeBoard(),
    nextPiece = nextPiece,
    level = level,
    lines = lines,
    INPUT_TIMELINE = INPUT_TIMELINE
  )
  print("Requesting precompute")
  response = requests.get(requestStr)
  print("Initial ack:", response.text)
  waitingOnAsync = True


def startTestCapture(onFrameCallback):
  testPieces = ['T', 'L', 'I', 'O', 'J', 'S', 'Z']
  countdown = 10
  lastSeen = None

  while len(testPieces) > 0:
    # Maybe update the last seen piece
    if countdown == 0:
      lastSeen = testPieces.pop()
      countdown = 38

    # Always provide a capture of its most recently seen state    
    if lastSeen == None:
      onFrameCallback(EMPTY_BOARD, "-")
    else:
      onFrameCallback(EMPTY_BOARD, lastSeen)
    countdown -= 1
    time.sleep(0.03333333)
    

def start():
  global isFirstPiece, frameQueue, currentPiece, nextPiece, level, lines, currentBoard, framesSinceLastPlacement
  isFirstPiece = True
  frameQueue = []
  currentPiece = None
  nextPiece = None
  level = STARTING_LEVEL
  currentBoard = None
  framesSinceLastPlacement = 0

  # Start the capture and the socket thread
  socketWorker = Thread(target=socketThread)
  socketWorker.daemon = True
  socketWorker.start()
  # video_capture.startCapture(onFrameCallback)

  time.sleep(0.5)
  # Send the starting inputs
  sendFrames("B....B.....B......." + FIRST_PIECE_PLACEMENT)
  time.sleep(1)

  startTestCapture(onFrameCallback)


# socketWorker = Thread(target=socketThread)
# socketWorker.daemon = True
# # socketWorker.start()
# for i in range(10):
#   st = time.time()
#   # sendFrames("A")
#   kk = requests.get("http://127.0.0.1:8080/ping")
#   print(time.time() - st)
#   time.sleep(1)
start()