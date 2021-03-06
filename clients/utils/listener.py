import requests
import zmq

#from  mongrel2.handler import Connection 
import time
import json
import sys

from websocket import create_connection

def listener(filtered, use_web_socket=True):
    
    if use_web_socket:
        socket = create_connection("ws://localhost:6767/watch/")
    else:
        context = zmq.Context()
        socket = context.socket(zmq.SUB)

        port = "9950"
        socket.connect ("tcp://127.0.0.1:%s" % port)
        socket.setsockopt(zmq.SUBSCRIBE, '')

    total_value = 0
    while True:
        message = socket.recv()
        json_message = json.loads(message);
        if json_message.get("type", None) == "PING":
            socket.send("PONG")

        if json_message.get("type", None) not in filtered:
            print message, ","
            #raw_input()
        if json_message.get("type", None) == "SHUTDOWN":
            return 

if __name__ == "__main__":
    filtered = sys.argv[1:]
    listener(filtered)