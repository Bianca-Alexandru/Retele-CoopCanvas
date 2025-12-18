DEPENDENCIES
------------
Client: Requires SDL2 libraries (sudo apt-get install libsdl2-dev libsdl2-image-dev)
        Requires brushes.h, ui.h, undo.h, and RawInput.h in the same folder.
        Requires ui.json in the same folder for the animated menu.
Server: Standard C++ libraries.
        Requires brushes.h in the same folder.

COMPILATION
-----------
1. Client:
   g++ client.cpp -o client -lSDL2 -lpthread

2. Server:
   g++ server.cpp -o server -lpthread -DSERVER_SIDE

   (-DSERVER_SIDE flag disables SDL2 dependency in brushes.h)

USAGE
-----
1. Start the Server:
   ./server

2. Start Clients:
   ./client [server_ip]
   sudo ./client [server_ip] --nuclear   (Use sudo if pen pressure is not detected)