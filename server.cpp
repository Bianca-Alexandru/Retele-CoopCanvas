/* 
   TCP/UDP Hybrid Server - Shared Canvas
   
   Protocol Scheme:
   UDP: DRAW, CURSOR, UNDO, REDO (Fast, high volume updates)
   TCP: LOGIN, CONNECT, COLOR, BRUSH (Reliable control messages)
*/

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <pthread.h>
#include <vector>
#include <string>
#include <algorithm>
#include <stdint.h>
#include <SDL2/SDL.h>
#include "brushes.h"

#define PORT 6769
#define WIDTH 640
#define HEIGHT 480

extern int errno;

/* --- PROTOCOL STRUCTURES --- */

// Message Types
enum MsgType {
    MSG_DRAW = 1,
    MSG_CURSOR = 2,
    MSG_UNDO = 3,
    MSG_REDO = 4,
    MSG_LOGIN = 5,
    MSG_CONNECT = 6,
    MSG_COLOR = 7,
    MSG_BRUSH = 8,
    MSG_LINE = 9
};

// UDP Message Structure (Draw, Cursor, Undo, Redo)
struct UDPMessage {
    int type;           // MSG_DRAW, MSG_CURSOR, MSG_UNDO, MSG_REDO
    int x, y;           // Coordinates (Start for Line)
    int ex, ey;         // End Coordinates (for Line)
    int id_brush;       // Brush ID
    int size;           // Brush Size
    SDL_Color color;    // Color
    float pressure;     // Pressure (0.0 - 1.0)
    int layer;          // Layer ID
};

// TCP Message Structure (Login, Connect, Color, Brush)
struct TCPMessage {
    int type;           // MSG_LOGIN, MSG_CONNECT, MSG_COLOR, MSG_BRUSH
    char username[64];  // For Login
    int id_canvas;      // For Connect
    SDL_Color color;    // For Color update
    int id_brush;       // For Brush update
};

/* --- GLOBAL STATE --- */

Pixel canvas[WIDTH][HEIGHT];
std::vector<int> tcp_clients;
std::vector<struct sockaddr_in> udp_clients; // List of known UDP clients
pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
std::vector<Brush*> availableBrushes;

int udp_socket; // Global UDP socket

/* --- HELPER FUNCTIONS --- */

void init_canvas() {
    for(int x=0; x<WIDTH; x++) {
        for(int y=0; y<HEIGHT; y++) {
            canvas[x][y] = {255, 255, 255, 255}; // White
        }
    }
}

// Helper to compare sockaddr_in
bool is_same_address(struct sockaddr_in a, struct sockaddr_in b) {
    return (a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port);
}

void add_udp_client(struct sockaddr_in client_addr) {
    pthread_mutex_lock(&state_mutex);
    bool found = false;
    for(const auto& addr : udp_clients) {
        if(is_same_address(addr, client_addr)) {
            found = true;
            break;
        }
    }
    if(!found) {
        udp_clients.push_back(client_addr);
        printf("[UDP] New client registered: %s:%d\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
    }
    pthread_mutex_unlock(&state_mutex);
}

void broadcast_udp(UDPMessage msg, struct sockaddr_in sender) {
    pthread_mutex_lock(&state_mutex);
    for(const auto& client_addr : udp_clients) {
        // Don't send back to sender
        if(!is_same_address(client_addr, sender)) {
            sendto(udp_socket, &msg, sizeof(UDPMessage), 0, (struct sockaddr*)&client_addr, sizeof(client_addr));
        }
    }
    pthread_mutex_unlock(&state_mutex);
}

void draw_line_on_canvas(int x0, int y0, int x1, int y1, Pixel color) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;

    while (1) {
        if (x0 >= 0 && x0 < WIDTH && y0 >= 0 && y0 < HEIGHT) {
            canvas[x0][y0] = color;
        }
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* --- UDP SERVER LOGIC --- */

void* udp_listener_thread(void* arg) {
    UDPMessage msg;
    struct sockaddr_in sender_addr;
    socklen_t len = sizeof(sender_addr);

    printf("[UDP] Listener thread started.\n");

    while(1) {
        int bytes = recvfrom(udp_socket, &msg, sizeof(UDPMessage), 0, (struct sockaddr*)&sender_addr, &len);
        if(bytes > 0) {
            // 1. Register client if new
            add_udp_client(sender_addr);

            // 2. Process Message
            if(msg.type == MSG_DRAW) {
                // Update Canvas
                pthread_mutex_lock(&state_mutex);
                auto setPixel = [&](int px, int py, Pixel c) {
                    if(px >= 0 && px < WIDTH && py >= 0 && py < HEIGHT) {
                        canvas[px][py] = c;
                    }
                };
                if(msg.id_brush >= 0 && msg.id_brush < availableBrushes.size()) {
                    availableBrushes[msg.id_brush]->paint(msg.x, msg.y, msg.color, msg.size, setPixel);
                }
                pthread_mutex_unlock(&state_mutex);
                
                // Broadcast to others
                broadcast_udp(msg, sender_addr);
            }
            else if(msg.type == MSG_LINE) {
                // Update Canvas with Line
                pthread_mutex_lock(&state_mutex);
                
                int x0 = msg.x, y0 = msg.y;
                int x1 = msg.ex, y1 = msg.ey;
                int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
                int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
                int err = dx + dy, e2;
                
                auto setPixel = [&](int px, int py, Pixel c) {
                    if(px >= 0 && px < WIDTH && py >= 0 && py < HEIGHT) {
                        canvas[px][py] = c;
                    }
                };

                while (1) {
                    if(msg.id_brush >= 0 && msg.id_brush < availableBrushes.size()) {
                        availableBrushes[msg.id_brush]->paint(x0, y0, msg.color, msg.size, setPixel);
                    }
                    if (x0 == x1 && y0 == y1) break;
                    e2 = 2 * err;
                    if (e2 >= dy) { err += dy; x0 += sx; }
                    if (e2 <= dx) { err += dx; y0 += sy; }
                }
                
                pthread_mutex_unlock(&state_mutex);
                
                // Broadcast to others
                broadcast_udp(msg, sender_addr);
            }
            else if(msg.type == MSG_CURSOR) {
                // Just broadcast cursor position
                broadcast_udp(msg, sender_addr);
            }
            // Handle UNDO/REDO...
        }
    }
    return NULL;
}

/* --- TCP SERVER LOGIC --- */

void send_canvas_history(int client_sock) {
    pthread_mutex_lock(&state_mutex);
    size_t total_size = WIDTH * HEIGHT * sizeof(Pixel);
    size_t sent = 0;
    char* ptr = (char*)canvas;
    
    while(sent < total_size) {
        int w = write(client_sock, ptr + sent, total_size - sent);
        if(w <= 0) break;
        sent += w;
    }
    pthread_mutex_unlock(&state_mutex);
}

void* tcp_client_session(void *arg) {
    int client_sock = *((int*)arg);
    free(arg);
    
    pthread_detach(pthread_self());
    
    // Register TCP client
    pthread_mutex_lock(&state_mutex);
    tcp_clients.push_back(client_sock);
    pthread_mutex_unlock(&state_mutex);

    printf("[TCP] Client connected (Socket %d)\n", client_sock);

    // Send initial canvas state
    send_canvas_history(client_sock);

    TCPMessage msg;
    while(1) {
        int bytes = read(client_sock, &msg, sizeof(TCPMessage));
        if(bytes <= 0) {
            printf("[TCP] Client disconnected (Socket %d)\n", client_sock);
            break;
        }

        // Handle TCP Messages
        switch(msg.type) {
            case MSG_LOGIN:
                printf("[TCP] Login: %s\n", msg.username);
                break;
            case MSG_CONNECT:
                printf("[TCP] Connect to Canvas ID: %d\n", msg.id_canvas);
                break;
            case MSG_COLOR:
                printf("[TCP] Color changed.\n");
                break;
            case MSG_BRUSH:
                printf("[TCP] Brush changed to ID: %d\n", msg.id_brush);
                break;
        }
    }

    // Cleanup
    close(client_sock);
    pthread_mutex_lock(&state_mutex);
    tcp_clients.erase(std::remove(tcp_clients.begin(), tcp_clients.end(), client_sock), tcp_clients.end());
    pthread_mutex_unlock(&state_mutex);
    
    return NULL;
}

/* --- JSON EXPORT (Isolated) --- */

static const char* b64_table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const unsigned char* data, size_t len) {
    std::string out;
    out.reserve(4 * ((len + 2) / 3));
    int val = 0, valb = -6;
    for (size_t i = 0; i < len; i++) {
        val = (val << 8) + data[i];
        valb += 8;
        while (valb >= 0) {
            out.push_back(b64_table[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(b64_table[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

void save_canvas_json() {
    std::vector<uint32_t> buffer;
    buffer.reserve(WIDTH * HEIGHT);
    
    for(int y=0; y<HEIGHT; y++) {
        for(int x=0; x<WIDTH; x++) {
            Pixel p = canvas[x][y];
            uint32_t packed = (p.r << 24) | (p.g << 16) | (p.b << 8) | p.a;
            buffer.push_back(packed);
        }
    }

    std::string b64 = base64_encode((const unsigned char*)buffer.data(), buffer.size() * sizeof(uint32_t));

    printf("{\n");
    printf("  \"width\": %d,\n", WIDTH);
    printf("  \"height\": %d,\n", HEIGHT);
    printf("  \"data\": \"%s\"\n", b64.c_str());
    printf("}\n");
}

/* --- MAIN --- */

int main() {
    init_canvas();
    
    // Initialize Brushes
    availableBrushes.push_back(new RoundBrush());
    availableBrushes.push_back(new SquareBrush());

    // 1. Setup TCP Socket
    int tcp_sd = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_sd == -1) { perror("TCP socket"); return 1; }
    
    int on = 1;
    setsockopt(tcp_sd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    
    struct sockaddr_in server_addr;
    bzero(&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(PORT);
    
    if (bind(tcp_sd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) { perror("TCP bind"); return 1; }
    if (listen(tcp_sd, 5) == -1) { perror("TCP listen"); return 1; }

    // 2. Setup UDP Socket
    udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_socket == -1) { perror("UDP socket"); return 1; }
    
    // Bind UDP to the same port (or different, but same is fine for different protocols)
    if (bind(udp_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) { perror("UDP bind"); return 1; }

    printf("[Server] Listening on TCP/UDP port %d...\n", PORT);

    // 3. Start UDP Listener Thread
    pthread_t udp_th;
    pthread_create(&udp_th, NULL, &udp_listener_thread, NULL);

    // 4. Accept TCP Connections
    while (1) {
        struct sockaddr_in from;
        socklen_t length = sizeof(from);
        
        int client = accept(tcp_sd, (struct sockaddr *)&from, &length);
        if (client < 0) {
            perror("Accept error");
            continue;
        }

        int* arg = (int*)malloc(sizeof(int));
        *arg = client;
        
        pthread_t tcp_th;
        pthread_create(&tcp_th, NULL, &tcp_client_session, arg);
    }
}
