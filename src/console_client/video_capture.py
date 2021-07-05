import time
import cv2 as cv
import numpy
import math

def pixelIsBlack(frame, x, y):
    x = round(x)
    y = round(y)

    threshold = 40
    [r, g, b] = frame[y][x]
    return r < threshold and g < threshold and b < threshold

def identifyStartingPiece(board):
    L_PIECE = "00001110000000100000"
    T_PIECE = "00001110000000010000"
    I_PIECE = "00011110000000000000"
    J_PIECE = "00001110000000001000"
    Z_PIECE = "00001100000000011000"
    S_PIECE = "00000110000000110000"
    O_PIECE = "00001100000000110000"
    pieces = [(I_PIECE, "I"), (O_PIECE, "O"), (L_PIECE, "L"), (J_PIECE, "J"), (T_PIECE, "T"), (S_PIECE, "S"), (Z_PIECE, "Z")]

    print(board[0:2])

    for piece, id in pieces:
        # Get the 'distance' from each piece
        distance = 0
        for i in range(len(piece)):
            pixelOn = board[math.floor(i / 10)][i % 10] == 1
            referencePixelOn = piece[i] == "1"
            if pixelOn != referencePixelOn:
                distance += 1
        if distance == 0:
            return id

    raise Exception("Couldn't read initial piece")

def identifyPiece(nextBoxArray):
    L_PIECE = ".XXXXXX..XXXXXX..XX......XX....."
    T_PIECE = ".XXXXXX..XXXXXX....XX......XX..."
    I_PIECE = "........XXXXXXXXXXXXXXXX........"
    J_PIECE = ".XXXXXX..XXXXXX......XX......XX."
    Z_PIECE = ".XXXX....XXXX......XXXX....XXXX."
    S_PIECE = "...XXXX....XXXX..XXXX....XXXX..."
    O_PIECE = "..XXXX....XXXX....XXXX....XXXX.."
    pieces = [(I_PIECE, "I"), (O_PIECE, "O"), (L_PIECE, "L"), (J_PIECE, "J"), (T_PIECE, "T"), (S_PIECE, "S"), (Z_PIECE, "Z")]

    closestPiece = ""
    closestDist = 99999
    for piece, id in pieces:
        # Get the 'distance' from each piece
        distance = 0
        for i in range(len(piece)):
            pixelOn = nextBoxArray[math.floor(i / 8)][i % 8] == 1
            referencePixelOn = piece[i] == "X"
            if pixelOn != referencePixelOn:
                distance += 1
        # print("Distance to piece", id, distance)
        if distance < closestDist:
            closestPiece = id,
            closestDist = distance

    return closestPiece[0]


def parseNextBox(frame):
    nextBoxX = 247 + 222
    nextBoxY = 86 + 130
    nextBoxWidth = 75
    nextBoxHeight = 63
    squareHeight = nextBoxHeight / 8
    squareWidth = nextBoxWidth / 8
    
    nextBox = []
    for row in range(8):
        newRow = []
        for col in range(8):
            xcoord = round(nextBoxX + squareWidth * (0.5 + col))
            ycoord = round(nextBoxY + squareHeight * (0.5 + row))
            cellFull = not pixelIsBlack(frame, xcoord, ycoord)
            # print(ycoord, xcoord, frame[ycoord, xcoord])

            newRow.append(1 if cellFull else 0)
        nextBox.append(newRow)
    return nextBox


def parseBoard(frame):
    boardX = 247
    boardY = 86
    boardWidth = 188
    boardHeight = 330
    squareHeight = boardHeight / 20
    squareWidth = boardWidth / 10

    board = []

    for row in range(20):
        newRow = []
        for col in range(10):
            xcoord = round(boardX + squareWidth * (0.5 + col))
            ycoord = round(boardY + squareHeight * (0.5 + row))
            cellFull = not pixelIsBlack(frame, xcoord, ycoord)
            # print(ycoord, xcoord, frame[ycoord, xcoord])

            newRow.append(1 if cellFull else 0)
        board.append(newRow)
    
    return board


def processFrame(frame):
    startTime = time.time()
    board = parseBoard(frame)
    nextBox = parseNextBox(frame)

    if numpy.sum(nextBox[3]) > 0: # All pieces have a cell filled in the middle row
        nextPieceId = identifyPiece(nextBox[2:7])
    else:
        print("\nNo data this frame")
        return (None, None)

    # # Print board
    # for row in range(20):
    #     rowStr = ""
    #     for col in range(10):
    #         rowStr += "." if board[row][col] == 0 else "X"
    #     print(rowStr)

    # # Print next box
    # for row in range(8):
    #     rowStr = ""
    #     for col in range(8):
    #         rowStr += "." if nextBox[row][col] == 0 else "X"
    #     print(rowStr)
    # print("Time elapsed:", round(time.time() - startTime, 4))

    # Print next piece ID
    # print("\nNext Piece identified as:", nextPieceId)

    return (board, nextPieceId)


def startCapture(onFrameCallback):
    cap = cv.VideoCapture(1)
    if not cap.isOpened():
        print("Cannot open camera")
        exit()
    # Define the codec and create VideoWriter object
    fourcc = cv.VideoWriter_fourcc(*'DIVX')
    out = cv.VideoWriter('output.avi', fourcc, 30.0, (640,  480))
    
    # lastFrameTime = time.time()

    while True:
        # immtime = time.time()
        ret, frame = cap.read()
        # print("capture time", time.time() - immtime)
        if not ret:
            print("Can't receive frame (stream end?). Exiting ...")
            break
        
        # print("FRAME TIME:", time.time() - lastFrameTime)
        # lastFrameTime = time.time()

        # comptime = time.time()
        (board, nextPieceId) = processFrame(frame)
        if board != None:
            onFrameCallback(board, nextPieceId)
            # print("COMP TIME", time.time() - comptime)
        # cv.imwrite("output image.png", frame[216:279])

        out.write(frame)
        # cv.imshow('frame', frame)
        # if cv.waitKey(1) == ord('q'):
        #     break
    # When everything done, release the capture
    cap.release()
    cv.destroyAllWindows()

if __name__ == '__main__':
    startCapture(lambda x,y:0)

