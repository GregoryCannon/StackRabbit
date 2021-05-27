import socket
import time

HOST = '127.0.0.1'  # The server's hostname or IP address
# HOST = '192.168.1.5'
PORT = 6000        # The port used by the server

inputSequences = ["F...L...L...L...", "..............", "A...A.................", "B......................L"]

with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
    s.connect((HOST, PORT))
    
    for sequence in inputSequences:
        print("Sending:", sequence)
        s.send(sequence.encode("utf-8"))
        response = s.recv(5).decode("utf-8")
        print("Got response", response)
        if (response != "Ack"):
            raise Exception("Request not acknowledged by the server")
        time.sleep(0.5)

    s.close()