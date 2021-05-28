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


'''
Clear out a floating piece if there is one.
    (The piece will always be in cols 3-6.)
'''
def clearFloatingPiece(board):
    startedClearing = False
    for row in range(20):
        if startedClearing:
            for col in range(3,7):
                board[row][col] = 0
        else:
            rowEmpty = True
            for col in range(3,7):
                if board[row][col] == 1:
                    rowEmpty = False
                    break
            if rowEmpty:
                startedClearing = True


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
        return

    # # Print board
    # for row in range(20):
    #     rowStr = ""
    #     for col in range(10):
    #         rowStr += "." if board[row][col] == 0 else "X"
    #     print(rowStr)

    # Print next box
    for row in range(8):
        rowStr = ""
        for col in range(8):
            rowStr += "." if nextBox[row][col] == 0 else "X"
        print(rowStr)
    print("Time elapsed:", round(time.time() - startTime, 4))

    # Print next piece ID
    print("\nNext Piece identified as:", nextPieceId)

    return (board, nextPieceId)


def startCapture(onFrameCallback):
    cap = cv.VideoCapture(1)
    if not cap.isOpened():
        print("Cannot open camera")
        exit()
    # Define the codec and create VideoWriter object
    # fourcc = cv.VideoWriter_fourcc(*'DIVX')
    # out = cv.VideoWriter('output.avi', fourcc, 30.0, (640,  480))
    
    for i in range(2000):
        ret, frame = cap.read()
        if not ret:
            print("Can't receive frame (stream end?). Exiting ...")
            break
        
        if i % 1 == 0:
            (board, nextPieceId) = processFrame(frame)
            onFrameCallback(board, nextPieceId)
            # cv.imwrite("output image.png", frame[216:279])

        # out.write(frame)
        # cv.imshow('frame', frame)
        if cv.waitKey(1) == ord('q'):
            break
    # When everything done, release the capture
    cap.release()
    cv.destroyAllWindows()

if __name__ == '__main__':
    startCapture()

