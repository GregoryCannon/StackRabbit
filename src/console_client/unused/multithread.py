from threading import Thread
from multiprocessing import Queue
import time
import socket

sharedVariable = 0
frameQueue = Queue()
frameQueue.put(".")
frameQueue.put(".")
frameQueue.put(".")

def makeServer():
  HOST = '127.0.0.1'  # Standard loopback interface address (localhost)
  PORT = 6000        # Port to listen on (non-privileged ports are > 1023)

  with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
    s.bind((HOST, PORT))
    s.listen()
    # while True:
    conn, addr = s.accept()
    requestsLeft = 1
    with conn: 
        print('Connected to by', addr)
        while requestsLeft > 0:
          data = conn.recv(1024)
          print(data)
          if not data:
              print("Connection closed.")
          conn.sendall(data)

def completeWork():
  global sharedVariable

  while(True):
    # try:
      if frameQueue.empty():
        # print("EMPTY QUEUE")
        continue
        # raise Exception("No frame queued up at input time") 
      newFrame = frameQueue.get()
      print("Playing frame:", newFrame)
      time.sleep(0.0166)
    # except:
    #   print("Closing worker thread due to main thread terminating")

def addWork():
  for inputChar in "F...L...L...L...L.........................":
    print("adding:", inputChar)
    frameQueue.put(inputChar)

  time.sleep(0.5)
  for inputChar in "I...R...R...R.........................L":
    frameQueue.put(inputChar)

  time.sleep(5)

workerThread = Thread(target=completeWork)
workerThread.daemon = True
# workerThread.start()

# Start the server on the main thread
makeServer()





# class Handler(socketserver.StreamRequestHandler):
#   def handle(self) -> None:
#       print("Connected to client at", self.request.getpeername())
#       self.wfile.write(b"Connection achieved")
#       for i in range(5):
#         req = self.rfile.read(2048)
#         print("Request:", req)

# server = socketserver.TCPServer((HOST, PORT), Handler)
# print("Started server")
# server.serve_forever()
