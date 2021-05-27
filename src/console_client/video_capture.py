import math
import cv2 as cv
cap = cv.VideoCapture(1)
if not cap.isOpened():
    print("Cannot open camera")
    exit()

# cap.set(cv.CAP_PROP_FRAME_WIDTH,480) 
# cap.set(cv.CAP_PROP_FRAME_HEIGHT,320)

def pixelIsBlack(frame, x, y):
    x = round(x)
    y = round(y)

    threshold = 40
    [r, g, b] = frame[y][x]
    return r < threshold and g < threshold and b < threshold

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
            print(ycoord, xcoord, frame[ycoord, xcoord])

            newRow.append(1 if cellFull else 0)
        board.append(newRow)
    
    return board

def processFrame(frame):
    #print(len(frame), len(frame[0]))
    #print(frame[100][100])
    
    # parseBoard(frame)

    board = parseBoard(frame)

    # Print board
    for row in range(20):
        rowStr = ""
        for col in range(10):
            rowStr += "." if board[row][col] == 0 else "X"
        print(rowStr)

def record():
    # Define the codec and create VideoWriter object
    fourcc = cv.VideoWriter_fourcc(*'DIVX')
    out = cv.VideoWriter('output.avi', fourcc, 30.0, (640,  480))
    
    for i in range(2000):
        ret, frame = cap.read()
        if not ret:
            print("Can't receive frame (stream end?). Exiting ...")
            break
        
        if i % 200 == 0:
            processFrame(frame)
            # map(lambda x: x[247:425]
            # cv.imwrite("starry_night.png", frame[86:416])

        out.write(frame)
        cv.imshow('frame', frame)
        if cv.waitKey(1) == ord('q'):
            break

record()

# When everything done, release the capture
cap.release()
cv.destroyAllWindows()