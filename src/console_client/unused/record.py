import numpy as np
import cv2 as cv
import time
cap = cv.VideoCapture(1)
# Define the codec and create VideoWriter object
fourcc = cv.VideoWriter_fourcc(*'XVID')
out = cv.VideoWriter('output.avi', fourcc, 30.0, (640,  480))

numFrames = 0
startTime = time.time()
while cap.isOpened():
    ret, frame = cap.read()

    if numFrames % 30 == 0:
      print(numFrames, round(time.time() - startTime, 3))
    numFrames += 1

    if not ret:
        print("Can't receive frame (stream end?). Exiting ...")
        break
    # frame = cv.flip(frame, 0)
    # write the flipped frame
    out.write(frame)
    cv.imshow('frame', frame)
    if cv.waitKey(1) == ord('q'):
        break
# Release everything if job is finished
cap.release()
out.release()
cv.destroyAllWindows()