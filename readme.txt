* Precompiled versions of both server and client have been provided *
* canvas.json will be created by the server if it doesn't exist but it is also provided as a clear example of how the data is kept *

DEPENDENCIES
------------
Client: Requires SDL2 library (sudo apt-get install libsdl2-dev)
        Requires brushes.h and ui.h in the same folder
Server: Standard C++ libraries only
        Requires brushes.h in the same folder

COMPILATION
-----------
1. Client:
   g++ client.cpp -o client -lSDL2 -lpthread

2. Server:
   g++ server.cpp -o server -lpthread -DSERVER_SIDE

   (-DSERVER_SIDE flag disables sdl2 dependency from brushes.h)

USAGE
-----
1. Start the Server first:
   ./server

2. Start one or more Clients:
   ./client [server_ip]

   Examples:
   ./client 127.0.0.1       //localhost
   ./client 10.100.0.30     //FII hosting

   //Port is hardcoded for ease of use
