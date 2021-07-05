from threading import Thread
from multiprocessing import Queue
import time
import socket

HOST = '127.0.0.1'
PORT = 6000

'''
Code for the raspberry pi that handles two tasks:
 - Networking with the python client on the main computer to get notified of the inputs
 - Execute the inputs with high time accuracy
'''

# Shared resources between threads
frameQueue = Queue()
serverReady = False

'''
Main function for the networking thread
'''
def startServer():
    global serverReady
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind((HOST, PORT))
        s.listen()
        print("Listening on port", PORT)
        serverReady = True

        # Accept only one client
        conn, addr = s.accept()
        with conn: 
            print('Connected to by', addr)
            # Read messages synchronously forever
            while True:
                data = conn.recv(2048)
                if not data:
                    break
                frames = data.decode("utf-8")
                # Add frames to queue
                for c in frames:
                    frameQueue.put(c)
                print(frames)
                conn.send(b'Ack')


def executeInput(inputChar):
  print("Executing input:", inputChar)


'''
Main function for the timing & inputs thread
'''
def main():
    serverThread = Thread(target=startServer)
    serverThread.daemon = True
    serverThread.start()

    timeoutSecs = 200000
    frameLength = .0166 # in seconds
    startTime = time.time()

    while not serverReady:
        pass

    while time.time() < startTime + timeoutSecs:
        if frameQueue.empty():
            # executeInput("_")
            pass
        else:
            executeInput(frameQueue.get())

        time.sleep(frameLength)

    print("Timeout reached - ending all threads")

main()
