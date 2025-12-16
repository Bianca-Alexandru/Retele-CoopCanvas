/* 
   COOP CANVAS SERVER TLDR
   
   -Main thread accepts TCP connections and routes clients
   -One thread per active canvas handles UDP traffic
   -Autosave thread periodically saves all canvases
   -Layer 0 is white background, Layer 1+ are transparent user layers
   -TCP used for Login, Save, Layer ops
   -UDP used for Draw, Line, Cursor
   -TCP port at 6769, UDP ports start at 6770 + canvasID
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
#include <map>
#include <algorithm>
#include <stdint.h>

#ifndef SERVER_SIDE
#define SERVER_SIDE
#endif
#include "brushes.h"

using namespace std;

#define PORT 6769
#define WIDTH 640
#define HEIGHT 480
#define MAX_LAYERS 15

extern int errno;

/* ****************************************************************************
   PROTOCOL STRUCTURES
   **************************************************************************** */

enum MsgType {
    MSG_LOGIN = 1,
    MSG_LOGOUT = 2,
    MSG_WELCOME = 3,
    MSG_CANVAS_DATA = 4,
    MSG_SAVE = 5,
    MSG_DRAW = 6,
    MSG_CURSOR = 7,
    MSG_LINE = 8,
    MSG_ERROR = 9,
    MSG_LAYER_ADD = 10,
    MSG_LAYER_DEL = 11,
    MSG_LAYER_SELECT = 12,
    MSG_LAYER_SYNC = 13,   // Full layer data sync (for undo/redo)
    MSG_LAYER_REORDER = 14, // Swap layers
    MSG_SIGNATURE = 15,      // New signature message
    MSG_LAYER_MOVE = 17
};

#define SIGNATURE_WIDTH 450
#define SIGNATURE_HEIGHT 150
#define MAX_SIGNATURE_SIZE (SIGNATURE_WIDTH * SIGNATURE_HEIGHT)

// TCP Message (packed for network)
struct TCPMessage {
    uint8_t  type;
    uint8_t  canvas_id;
    uint16_t data_len;
    uint8_t  layer_count;
    uint8_t  layer_id;
    uint8_t  user_id; // Added for signature tracking
    char     data[256]; // Reverted payload size
} __attribute__((packed));

// Specialized packet for login with signature
struct LoginPacket {
    uint8_t  type;        // MSG_LOGIN
    uint8_t  canvas_id;
    char     username[32];
    uint16_t sig_width;
    uint16_t sig_height;
    uint32_t sig_len;
    uint8_t  sig_data[MAX_SIGNATURE_SIZE]; // ~32KB
} __attribute__((packed));

// UDP Message (packed for network)
struct UDPMessage {
    uint8_t  type;
    uint8_t  brush_id;
    uint8_t  layer_id;
    int16_t  x;
    int16_t  y;
    int16_t  ex, ey;  // for line drawing
    uint8_t  r, g, b, a;
    uint8_t  size;
    uint8_t  pressure;  // 0-255 representing 0.0-1.0 pressure (for pen tablets)
} __attribute__((packed));

/*****************************************************************************
   LAYER STRUCTURE
 *****************************************************************************/

struct Layer {
    Pixel pixels[WIDTH][HEIGHT];
    bool dirty;
    string cached_b64;

    Layer() : dirty(true) {}

    void init_transparent() {
        for (int x = 0; x < WIDTH; x++) {
            for (int y = 0; y < HEIGHT; y++) {
                pixels[x][y] = {0, 0, 0, 0};
            }
        }
        dirty = true;
    }
    
    void init_white() {
        for (int x = 0; x < WIDTH; x++) {
            for (int y = 0; y < HEIGHT; y++) {
                pixels[x][y] = {255, 255, 255, 255};
            }
        }
        dirty = true;
    }
};

/*****************************************************************************
   CANVAS ROOM STRUCTURE
 *****************************************************************************/

struct ConnectedUser {
    int socket_fd;
    char username[32];
    uint8_t* signature_data; // Alpha channel bitmap
    int signature_len;
    uint8_t room_uid; // Unique ID (1-255) within the room
    
    ConnectedUser() : socket_fd(-1), signature_data(nullptr), signature_len(0), room_uid(0) {
        memset(username, 0, sizeof(username));
    }
    
    ~ConnectedUser() {
        if (signature_data) delete[] signature_data;
    }
};

struct CanvasRoom {
    int id;
    vector<Layer*> layers;
    
    int udp_socket;
    int udp_port;
    pthread_t thread;
    bool active;
    
    vector<struct sockaddr_in> udp_clients;
    vector<int> tcp_clients;
    map<int, ConnectedUser*> users; // Map socket_fd -> User info
    pthread_mutex_t mutex;
    bool dirty;
    
    void init(int canvas_id) {
        id = canvas_id;
        active = false;
        dirty = true;
        udp_socket = -1;
        udp_port = PORT + 1 + canvas_id;
        pthread_mutex_init(&mutex, NULL);
        
        Layer* paper = new Layer();
        paper->init_white();
        layers.push_back(paper);
        
        Layer* layer1 = new Layer();
        layer1->init_transparent();
        layers.push_back(layer1);
        
        printf("[Server][Canvas %d] Initialized with %zu layers (paper + 1 drawable)\n", id, layers.size());
    }
    
    void add_user(int fd, const char* name, const uint8_t* sig_data, int sig_len) {
        ConnectedUser* u = new ConnectedUser();
        u->socket_fd = fd;
        strncpy(u->username, name, 31);
        
        // Assign unique room_uid (1-255)
        // Find first available ID
        vector<bool> used_ids(256, false);
        for (auto const& [key, val] : users) {
            used_ids[val->room_uid] = true;
        }
        for (int i = 1; i < 256; i++) {
            if (!used_ids[i]) {
                u->room_uid = i;
                break;
            }
        }
        
        if (sig_data && sig_len > 0) {
            u->signature_len = sig_len;
            u->signature_data = new uint8_t[sig_len];
            memcpy(u->signature_data, sig_data, sig_len);
        }
        users[fd] = u;
        printf("[Server][Canvas %d] User %s added with signature (%d bytes), UID=%d\n", id, name, sig_len, u->room_uid);
    }
    
    void remove_user(int fd) {
        if (users.count(fd)) {
            delete users[fd];
            users.erase(fd);
        }
    }
    
    void add_layer() {
        if (layers.size() >= MAX_LAYERS) {
            printf("[Server][Canvas %d] Cannot add layer: max %d layers reached\n", id, MAX_LAYERS);
            return;
        }
        Layer* newLayer = new Layer();
        newLayer->init_transparent();
        layers.push_back(newLayer);
        printf("[Server][Canvas %d] Added layer #%zu (total: %zu)\n", id, layers.size() - 1, layers.size());
    }

    void insert_layer(int layer_idx) {
        if (layers.size() >= MAX_LAYERS) {
            printf("[Server][Canvas %d] Cannot insert layer: max %d layers reached\n", id, MAX_LAYERS);
            return;
        }
        if (layer_idx <= 0 || layer_idx > (int)layers.size()) {
             add_layer();
             return;
        }
        
        Layer* newLayer = new Layer();
        newLayer->init_transparent();
        layers.insert(layers.begin() + layer_idx, newLayer);
        printf("[Server][Canvas %d] Inserted layer at #%d (total: %zu)\n", id, layer_idx, layers.size());
    }
    
    void delete_layer(int layer_idx) {
        if (layer_idx <= 0 || layer_idx >= (int)layers.size()) {
            printf("[Server][Canvas %d] Cannot delete layer %d: invalid index\n", id, layer_idx);
            return;
        }
        if (layers.size() <= 2) {
            printf("[Server][Canvas %d] Cannot delete layer %d: must keep at least 1 drawable\n", id, layer_idx);
            return;
        }
        delete layers[layer_idx];
        layers.erase(layers.begin() + layer_idx);
        printf("[Server][Canvas %d] Deleted layer #%d (remaining: %zu)\n", id, layer_idx, layers.size());
    }

    void reorder_layer(int old_idx, int new_idx) {
        if (old_idx <= 0 || old_idx >= (int)layers.size() || new_idx <= 0 || new_idx >= (int)layers.size()) return;
        if (old_idx == new_idx) return;
        
        Layer* l = layers[old_idx];
        layers.erase(layers.begin() + old_idx);
        layers.insert(layers.begin() + new_idx, l);
        printf("[Server][Canvas %d] Moved layer %d to %d\n", id, old_idx, new_idx);
    }
    
    void flatten_to_buffer(Pixel* buffer) {
        for (int x = 0; x < WIDTH; x++) {
            for (int y = 0; y < HEIGHT; y++) {
                buffer[x * HEIGHT + y] = {255, 255, 255, 255};
            }
        }
        for (size_t l = 1; l < layers.size(); l++) {
            for (int x = 0; x < WIDTH; x++) {
                for (int y = 0; y < HEIGHT; y++) {
                    Pixel src = layers[l]->pixels[x][y];
                    if (src.a > 0) {
                        Pixel& dst = buffer[x * HEIGHT + y];
                        float srcA = src.a / 255.0f;
                        float dstA = dst.a / 255.0f;
                        float outA = srcA + dstA * (1 - srcA);
                        if (outA > 0) {
                            dst.r = (src.r * srcA + dst.r * dstA * (1 - srcA)) / outA;
                            dst.g = (src.g * srcA + dst.g * dstA * (1 - srcA)) / outA;
                            dst.b = (src.b * srcA + dst.b * dstA * (1 - srcA)) / outA;
                            dst.a = outA * 255;
                        }
                    }
                }
            }
        }
    }
};

/*****************************************************************************
   GLOBAL STATE
 *****************************************************************************/

map<int, CanvasRoom*> canvases;  // On-demand canvas creation
pthread_mutex_t canvases_mutex = PTHREAD_MUTEX_INITIALIZER;
vector<Brush*> availableBrushes;

// Track drawing state per client for less spammy logs
map<string, bool> client_drawing;

/*****************************************************************************
   HELPER FUNCTIONS
 *****************************************************************************/

bool is_same_address(struct sockaddr_in a, struct sockaddr_in b) {
    return (a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port);
}

string addr_to_key(struct sockaddr_in addr) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s:%d", inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
    return string(buf);
}

// Helper: Write all data to socket (handles partial writes)
bool write_all(int sock, const void* data, size_t len) {
    const uint8_t* ptr = (const uint8_t*)data;
    size_t sent = 0;
    while (sent < len) {
        ssize_t w = write(sock, ptr + sent, len - sent);
        if (w <= 0) return false;
        sent += w;
    }
    return true;
}

// Helper: Broadcast TCPMessage to all clients in a room (optionally exclude one)
void broadcast_tcp(CanvasRoom* room, const TCPMessage& msg, int exclude_sock = -1) {
    for (int sock : room->tcp_clients) {
        if (sock != exclude_sock) {
            write(sock, &msg, sizeof(TCPMessage));
        }
    }
}

// Forward declaration
CanvasRoom* get_or_create_canvas(int canvas_id);

/*****************************************************************************
   UDP MESSAGE HANDLERS
 *****************************************************************************/

// Broadcast UDP message to all clients except sender
int broadcast_udp(CanvasRoom* room, const UDPMessage& msg, const sockaddr_in& sender_addr) {
    int count = 0;
    for (const auto& client : room->udp_clients) {
        if (!is_same_address(client, sender_addr)) {
            sendto(room->udp_socket, &msg, sizeof(UDPMessage), 0,
                   (struct sockaddr*)&client, sizeof(client));
            count++;
        }
    }
    return count;
}

void handle_draw(CanvasRoom* room, const UDPMessage& msg, const sockaddr_in& sender_addr,
                 int canvas_id, const string& client_key) {
    int layer_idx = msg.layer_id;
    if (layer_idx <= 0 || layer_idx >= (int)room->layers.size()) {
        layer_idx = 1;
    }
    
    Pixel col = {msg.r, msg.g, msg.b, msg.a};
    
    // Only log when drawing starts
    if (!client_drawing[client_key]) {
        client_drawing[client_key] = true;
        printf("[Server][Canvas %d][UDP] DRAW START: client=%s layer=%d brush=%d size=%d color=RGBA(%d,%d,%d,%d)\n",
               canvas_id, client_key.c_str(), layer_idx, msg.brush_id, msg.size, msg.r, msg.g, msg.b, msg.a);
    }
    
    pthread_mutex_lock(&room->mutex);
    room->dirty = true;
    auto setPixel = [&](int px, int py, Pixel c) {
        if (px >= 0 && px < WIDTH && py >= 0 && py < HEIGHT) {
            room->layers[layer_idx]->pixels[px][py] = c;
            room->layers[layer_idx]->dirty = true;
        }
    };
    if (msg.brush_id < (int)availableBrushes.size()) {
        int angle = msg.ex;
        availableBrushes[msg.brush_id]->paint(msg.x, msg.y, col, msg.size, msg.pressure, angle, setPixel);
    }
    broadcast_udp(room, msg, sender_addr);
    pthread_mutex_unlock(&room->mutex);
}

void handle_cursor(CanvasRoom* room, const UDPMessage& msg, const sockaddr_in& sender_addr,
                   int canvas_id, const string& client_key) {
    // Cursor also marks end of drawing
    if (client_drawing[client_key]) {
        client_drawing[client_key] = false;
        printf("[Server][Canvas %d][UDP] DRAW END: client=%s\n", canvas_id, client_key.c_str());
    }
    
    // Inject room_uid into brush_id field for cursor tracking
    UDPMessage fwd = msg;
    pthread_mutex_lock(&room->mutex);
    broadcast_udp(room, fwd, sender_addr);
    pthread_mutex_unlock(&room->mutex);
}

void handle_line(CanvasRoom* room, const UDPMessage& msg, const sockaddr_in& sender_addr,
                 int canvas_id, const string& client_key) {
    int layer_idx = msg.layer_id;
    if (layer_idx <= 0 || layer_idx >= (int)room->layers.size()) {
        layer_idx = 1;
    }
    
    Pixel col = {msg.r, msg.g, msg.b, msg.a};
    printf("[Server][Canvas %d][UDP] LINE: client=%s from=(%d,%d) to=(%d,%d) layer=%d brush=%d\n",
           canvas_id, client_key.c_str(), msg.x, msg.y, msg.ex, msg.ey, layer_idx, msg.brush_id);
    
    pthread_mutex_lock(&room->mutex);
    room->dirty = true;
    
    int x0 = msg.x, y0 = msg.y;
    int x1 = msg.ex, y1 = msg.ey;
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;
    
    // Calculate angle for the line
    int angle = (int)(atan2(msg.ey - msg.y, msg.ex - msg.x) * 180.0 / M_PI);
    
    auto setPixel = [&](int px, int py, Pixel c) {
        if (px >= 0 && px < WIDTH && py >= 0 && py < HEIGHT) {
            room->layers[layer_idx]->pixels[px][py] = c;
            room->layers[layer_idx]->dirty = true;
        }
    };

    while (true) {
        if (msg.brush_id < (int)availableBrushes.size()) {
            availableBrushes[msg.brush_id]->paint(x0, y0, col, msg.size, msg.pressure, angle, setPixel);
        }
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
    
    int bc = broadcast_udp(room, msg, sender_addr);
    pthread_mutex_unlock(&room->mutex);
    printf("[Server][Canvas %d][UDP] LINE broadcast to %d clients\n", canvas_id, bc);
}

/*****************************************************************************
   CANVAS UDP THREAD
 *****************************************************************************/

void* canvas_udp_thread(void* arg) {
    int canvas_id = *((int*)arg);
    free(arg);
    
    CanvasRoom* room = get_or_create_canvas(canvas_id);
    
    printf("[Server][Canvas %d][UDP] Thread started on port %d\n", canvas_id, room->udp_port);
    
    UDPMessage msg;
    struct sockaddr_in sender_addr;
    socklen_t len = sizeof(sender_addr);
    
    while (room->active) {
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        setsockopt(room->udp_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        
        int bytes = recvfrom(room->udp_socket, &msg, sizeof(UDPMessage), 0, 
                             (struct sockaddr*)&sender_addr, &len);
        
        if (bytes <= 0) continue;
        
        
        string client_key = addr_to_key(sender_addr);
        
        pthread_mutex_lock(&room->mutex);
        bool found = false;
        for (const auto& addr : room->udp_clients) {
            if (is_same_address(addr, sender_addr)) {
                found = true;
                break;
            }
        }
        if (!found) {
            room->udp_clients.push_back(sender_addr);
            printf("[Server][Canvas %d][UDP] New client: %s (total: %zu)\n", 
                   canvas_id, client_key.c_str(), room->udp_clients.size());
        }
        pthread_mutex_unlock(&room->mutex);
        
        if (msg.type == MSG_DRAW) {
            handle_draw(room, msg, sender_addr, canvas_id, client_key);
        }
        else if (msg.type == MSG_CURSOR) {
            handle_cursor(room, msg, sender_addr, canvas_id, client_key);
        }
        else if (msg.type == MSG_LINE) {
            handle_line(room, msg, sender_addr, canvas_id, client_key);
        }
    }
    
    printf("[Server][Canvas %d][UDP] Thread stopped\n", canvas_id);
    close(room->udp_socket);
    return NULL;
}

/*****************************************************************************
   START/STOP CANVAS THREAD
 *****************************************************************************/

CanvasRoom* get_or_create_canvas(int canvas_id) {
    pthread_mutex_lock(&canvases_mutex);
    
    if (canvases.find(canvas_id) == canvases.end()) {
        printf("[Server] Creating new canvas #%d on demand\n", canvas_id);
        CanvasRoom* room = new CanvasRoom();
        room->init(canvas_id);
        canvases[canvas_id] = room;
    }
    
    CanvasRoom* room = canvases[canvas_id];
    pthread_mutex_unlock(&canvases_mutex);
    return room;
}

bool start_canvas_thread(int canvas_id) {
    if (canvas_id < 0) {
        printf("[Server] ERROR: Invalid canvas_id %d\n", canvas_id);
        return false;
    }
    
    CanvasRoom* room = get_or_create_canvas(canvas_id);
    
    pthread_mutex_lock(&canvases_mutex);
    
    if (room->active) {
        printf("[Server][Canvas %d] Thread already running\n", canvas_id);
        pthread_mutex_unlock(&canvases_mutex);
        return true;
    }
    
    room->udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (room->udp_socket < 0) {
        perror("[Server] UDP socket failed");
        pthread_mutex_unlock(&canvases_mutex);
        return false;
    }
    
    struct sockaddr_in addr;
    bzero(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(room->udp_port);
    
    if (bind(room->udp_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        printf("[Server] ERROR: UDP bind failed for port %d\n", room->udp_port);
        close(room->udp_socket);
        pthread_mutex_unlock(&canvases_mutex);
        return false;
    }
    
    room->active = true;
    
    int* arg = (int*)malloc(sizeof(int));
    *arg = canvas_id;
    pthread_create(&room->thread, NULL, canvas_udp_thread, arg);
    
    printf("[Server][Canvas %d] Thread STARTED on UDP port %d\n", canvas_id, room->udp_port);
    
    pthread_mutex_unlock(&canvases_mutex);
    return true;
}

/*****************************************************************************
   PERSISTENCE - Single canvas.json with all canvases and layers
 *****************************************************************************/

static const string b64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// PackBits Compression (RLE variant)
// Header N:
// [0, 127]   -> (N+1) literal bytes follow
// [-127, -1] -> Repeat next byte (1-N) times (2 to 128 times)
// -128       -> No-op
vector<uint8_t> packbits_compress(const uint8_t* data, size_t len) {
    vector<uint8_t> out;
    size_t i = 0;
    while (i < len) {
        // Look for run
        size_t run_start = i;
        while (i + 1 < len && data[i] == data[i+1] && (i - run_start) < 127) {
            i++;
        }
        
        if (i > run_start) {
            // We have a run of (i - run_start + 1) bytes
            // Length is at least 2
            int count = (i - run_start + 1);
            out.push_back((uint8_t)(257 - count)); // -count + 1 + 256 = 257 - count
            out.push_back(data[run_start]);
            i++;
        } else {
            // Literal Run (Non-repeating sequence)
            size_t j = i;
            
            // Advance j until we hit a run of 3 identical bytes OR max literal length (128)
            while (j < len && (j - i) < 128) {
                if (j + 2 < len && data[j] == data[j+1] && data[j] == data[j+2]) {
                    break; // Found a run of 3, stop literal here
                }
                j++;
            }
            
            int count = (j - i);
            out.push_back((uint8_t)(count - 1)); // 0 means 1 literal byte
            
            for (size_t k = 0; k < (size_t)count; k++) {
                out.push_back(data[i + k]);
            }
            
            i = j;
        }
    }
    return out;
}

vector<uint8_t> packbits_decompress(const vector<uint8_t>& in) {
    vector<uint8_t> out;
    size_t i = 0;
    while (i < in.size()) {
        int8_t n = (int8_t)in[i++];
        if (n == -128) continue; // No-op
        
        if (n >= 0) {
            // 0 to 127: Copy N+1 bytes
            int count = n + 1;
            for (int k = 0; k < count && i < in.size(); k++) {
                out.push_back(in[i++]);
            }
        } else {
            // -1 to -127: Repeat next byte (1-N) times
            int count = 1 - n;
            if (i < in.size()) {
                uint8_t val = in[i++];
                for (int k = 0; k < count; k++) {
                    out.push_back(val);
                }
            }
        }
    }
    return out;
}

bool is_base64(unsigned char c) {
    return (isalnum(c) || (c == '+') || (c == '/'));
}

string base64_encode(const unsigned char* data, size_t len) {
    string out;
    out.reserve(4 * ((len + 2) / 3));
    int val = 0, valb = -6;
    for (size_t i = 0; i < len; i++) {
        val = (val << 8) + data[i];
        valb += 8;
        while (valb >= 0) {
            out.push_back(b64_chars[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(b64_chars[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

vector<unsigned char> base64_decode(const string& encoded_string) {
    int in_len = encoded_string.size();
    int i = 0, j = 0, in_ = 0;
    unsigned char char_array_4[4], char_array_3[3];
    vector<unsigned char> ret;

    while (in_len-- && encoded_string[in_] != '=' && is_base64(encoded_string[in_])) {
        char_array_4[i++] = encoded_string[in_]; in_++;
        if (i == 4) {
            for (i = 0; i < 4; i++)
                char_array_4[i] = b64_chars.find(char_array_4[i]);

            char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
            char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
            char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];

            for (i = 0; i < 3; i++)
                ret.push_back(char_array_3[i]);
            i = 0;
        }
    }

    if (i) {
        for (j = i; j < 4; j++) char_array_4[j] = 0;
        for (j = 0; j < 4; j++) char_array_4[j] = b64_chars.find(char_array_4[j]);

        char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
        char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
        char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];

        for (j = 0; j < i - 1; j++) ret.push_back(char_array_3[j]);
    }

    return ret;
}

string encode_layer(Layer* layer) {
    vector<uint32_t> buffer;
    buffer.reserve(WIDTH * HEIGHT);
    
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            Pixel p = layer->pixels[x][y];
            uint32_t packed = (p.r << 24) | (p.g << 16) | (p.b << 8) | p.a;
            buffer.push_back(packed);
        }
    }
    
    // Compress using PackBits
    vector<uint8_t> compressed = packbits_compress((const uint8_t*)buffer.data(), buffer.size() * sizeof(uint32_t));
    
    return base64_encode(compressed.data(), compressed.size());
}

void decode_layer(Layer* layer, const string& b64, int json_width, int json_height) {
    vector<unsigned char> compressed = base64_decode(b64);
    
    // Decompress using PackBits
    vector<uint8_t> data = packbits_decompress(compressed);
    
    uint32_t* pixels = (uint32_t*)data.data();
    int count = 0;
    int max_pixels = data.size() / sizeof(uint32_t);
    
    // Iterate over the stored dimensions to consume the stream correctly
    for (int y = 0; y < json_height; y++) {
        for (int x = 0; x < json_width; x++) {
            if (count < max_pixels) {
                uint32_t p = pixels[count++];
                
                // Only copy if within current bounds
                if (x < WIDTH && y < HEIGHT) {
                    layer->pixels[x][y].r = (p >> 24) & 0xFF;
                    layer->pixels[x][y].g = (p >> 16) & 0xFF;
                    layer->pixels[x][y].b = (p >> 8) & 0xFF;
                    layer->pixels[x][y].a = p & 0xFF;
                }
            }
        }
    }
}

void save_all_canvases() {
    // 1. Global Optimization: Check if ANYTHING is dirty
    bool any_dirty = false;
    for (auto& pair : canvases) {
        if (pair.second->dirty) {
            any_dirty = true;
            break;
        }
    }
    if (!any_dirty) return; // Silent return if nothing changed

    printf("\n[Server][Save] ========== SAVING DIRTY CANVASES ==========\n");
    
    FILE* f = fopen("canvas.json", "w");
    if (!f) {
        printf("[Server][Save] ERROR: Cannot open canvas.json\n");
        return;
    }
    
    fprintf(f, "{\n  \"version\": 2,\n  \"width\": %d,\n  \"height\": %d,\n  \"canvases\": [\n", WIDTH, HEIGHT);
    
    bool first_canvas = true;
    int saved_count = 0;
    
    for (auto& pair : canvases) {
        int c = pair.first;
        CanvasRoom* room = pair.second;
        
        // Check if has content
        bool has_content = false;
        for (size_t l = 1; l < room->layers.size() && !has_content; l++) {
            for (int x = 0; x < WIDTH && !has_content; x++) {
                for (int y = 0; y < HEIGHT && !has_content; y++) {
                    if (room->layers[l]->pixels[x][y].a > 0) has_content = true;
                }
            }
        }
        if (!has_content && !room->active) continue;
        
        if (!first_canvas) fprintf(f, ",\n");
        first_canvas = false;
        
        pthread_mutex_lock(&room->mutex);
        
        // Log only if this specific room changed
        if (room->dirty) {
            printf("[Server][Save] Saving Canvas #%d...\n", c);
        }
        
        fprintf(f, "    {\n      \"id\": %d,\n      \"layer_count\": %zu,\n      \"layers\": [\n", c, room->layers.size() - 1);
        
        for (size_t l = 1; l < room->layers.size(); l++) {
            Layer* layer = room->layers[l];
            
            // CACHING LOGIC
            if (layer->dirty || layer->cached_b64.empty()) {
                // Re-encode only if dirty
                layer->cached_b64 = encode_layer(layer);
                layer->dirty = false;
            }
            
            fprintf(f, "        {\"index\": %zu, \"data\": \"%s\"}%s\n", l, layer->cached_b64.c_str(), (l < room->layers.size() - 1) ? "," : "");
        }
        
        fprintf(f, "      ]\n    }");
        
        room->dirty = false; // Reset room dirty flag
        pthread_mutex_unlock(&room->mutex);
        saved_count++;
    }
    
    fprintf(f, "\n  ]\n}\n");
    fclose(f);
    
    printf("[Server][Save] Saved %d canvases\n", saved_count);
    printf("[Server][Save] ========== SAVE COMPLETE ==========\n\n");
}

void load_all_canvases() {
    printf("\n[Server][Load] ========== LOADING canvas.json ==========\n");
    
    FILE* f = fopen("canvas.json", "r");
    if (!f) {
        printf("[Server][Load] No canvas.json found - creating default...\n");
        // Create a default canvas if none exists
        CanvasRoom* room = get_or_create_canvas(0);
        save_all_canvases();
        return;
    }
    
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    printf("[Server][Load] File size: %ld bytes\n", fsize);
    
    char* buffer = (char*)malloc(fsize + 1);
    fread(buffer, 1, fsize, f);
    buffer[fsize] = 0;
    fclose(f);
    
    string json = buffer;
    free(buffer);
    
    int json_width = WIDTH;
    int json_height = HEIGHT;
    
    size_t w_pos = json.find("\"width\":");
    if (w_pos != string::npos) {
        json_width = atoi(json.c_str() + w_pos + 8);
    }
    
    size_t h_pos = json.find("\"height\":");
    if (h_pos != string::npos) {
        json_height = atoi(json.c_str() + h_pos + 9);
    }
    
    printf("[Server][Load] JSON Dimensions: %dx%d (Current: %dx%d)\n", json_width, json_height, WIDTH, HEIGHT);
    
    size_t pos = 0;
    while ((pos = json.find("\"id\":", pos)) != string::npos) {
        size_t id_start = pos + 5;
        while (id_start < json.size() && (json[id_start] == ' ' || json[id_start] == '\t')) id_start++;
        int canvas_id = atoi(json.c_str() + id_start);
        
        if (canvas_id < 0) { pos++; continue; }
        
        printf("[Server][Load] Found canvas #%d\n", canvas_id);
        
        // Create or get the canvas on demand
        CanvasRoom* room = get_or_create_canvas(canvas_id);
        
        size_t layers_start = json.find("\"layers\":", pos);
        if (layers_start == string::npos) { pos++; continue; }
        
        size_t layers_array_start = json.find("[", layers_start);
        if (layers_array_start == string::npos) { pos++; continue; }

        size_t layers_array_end = json.find("]", layers_array_start);
        if (layers_array_end == string::npos) layers_array_end = json.size();
        
        size_t layer_pos = layers_array_start;
        int layer_count = 0;
        
        while ((layer_pos = json.find("\"data\":", layer_pos)) != string::npos && layer_pos < layers_array_end) {
            size_t data_start = json.find("\"", layer_pos + 7);
            if (data_start == string::npos) break;
            data_start++;
            
            size_t data_end = json.find("\"", data_start);
            if (data_end == string::npos) break;
            
            string b64 = json.substr(data_start, data_end - data_start);
            
            while (room->layers.size() <= (size_t)(layer_count + 1)) {
                Layer* newLayer = new Layer();
                newLayer->init_transparent();
                room->layers.push_back(newLayer);
            }
            
            decode_layer(room->layers[layer_count + 1], b64, json_width, json_height);
            printf("[Server][Load] Canvas #%d Layer %d loaded\n", canvas_id, layer_count + 1);
            
            layer_count++;
            layer_pos = data_end;
        }
        
        printf("[Server][Load] Canvas #%d: %d drawable layers loaded\n", canvas_id, layer_count);
        pos = layers_array_end;
    }
    
    printf("[Server][Load] ========== LOAD COMPLETE ==========\n\n");
}

/*****************************************************************************
   AUTOSAVE THREAD
 *****************************************************************************/

void* autosave_thread(void* arg) {
    printf("[Server][Autosave] Thread started (interval: 60s)\n");
    while (1) {
        sleep(60);
        printf("[Server][Autosave] Timer triggered\n");
        pthread_mutex_lock(&canvases_mutex);
        save_all_canvases();
        pthread_mutex_unlock(&canvases_mutex);
    }
    return NULL;
}

/*****************************************************************************
   TCP SESSION HANDLER
 *****************************************************************************/

void send_canvas_to_client(int sock, int canvas_id) {
    CanvasRoom* room = get_or_create_canvas(canvas_id);
    
    printf("[Server][TCP] Sending canvas #%d to socket %d\n", canvas_id, sock);
    
    int layer_count = room->layers.size();
    write(sock, &layer_count, sizeof(int));
    printf("[Server][TCP] Sent layer_count: %d\n", layer_count);
    
    pthread_mutex_lock(&room->mutex);
    
    // Send each layer individually (skip layer 0 which is white paper)
    // Convert from column-major (pixels[x][y]) to row-major (y * WIDTH + x) for client
    uint8_t* buffer = new uint8_t[WIDTH * HEIGHT * 4];
    
    for (size_t l = 1; l < room->layers.size(); l++) {
        // Convert to row-major RGBA format
        for (int y = 0; y < HEIGHT; y++) {
            for (int x = 0; x < WIDTH; x++) {
                Pixel p = room->layers[l]->pixels[x][y];
                int idx = (y * WIDTH + x) * 4;
                buffer[idx + 0] = p.r;
                buffer[idx + 1] = p.g;
                buffer[idx + 2] = p.b;
                buffer[idx + 3] = p.a;
            }
        }
        
        if (write_all(sock, buffer, WIDTH * HEIGHT * 4)) {
            printf("[Server][TCP] Sent layer %zu (%d bytes)\n", l, WIDTH * HEIGHT * 4);
        } else {
            printf("[Server][TCP] Failed to send layer %zu\n", l);
        }
    }
    
    delete[] buffer;
    pthread_mutex_unlock(&room->mutex);
    
    printf("[Server][TCP] Sent canvas #%d complete\n", canvas_id);
}

void move_layer_buffer(Layer* layer, int dx, int dy) {
    if (!layer) return;
    if (dx == 0 && dy == 0) return;
    layer->dirty = true;

    // Use a temp buffer
    Pixel** temp = new Pixel*[WIDTH];
    for (int i = 0; i < WIDTH; i++) {
        temp[i] = new Pixel[HEIGHT];
        memset(temp[i], 0, HEIGHT * sizeof(Pixel)); // Clear to transparent
    }

    for (int x = 0; x < WIDTH; x++) {
        for (int y = 0; y < HEIGHT; y++) {
            // Calculate where this pixel comes FROM
            int srcX = x - dx;
            int srcY = y - dy;

            if (srcX >= 0 && srcX < WIDTH && srcY >= 0 && srcY < HEIGHT) {
                temp[x][y] = layer->pixels[srcX][srcY];
            }
        }
    }

    // Copy back and cleanup
    for (int i = 0; i < WIDTH; i++) {
        for (int j = 0; j < HEIGHT; j++) {
            layer->pixels[i][j] = temp[i][j];
        }
        delete[] temp[i];
    }
    delete[] temp;
}

void* tcp_client_session(void* arg) {
    int client_sock = *((int*)arg);
    free(arg);
    
    pthread_detach(pthread_self());
    
    printf("[Server][TCP] ===== Client connected (socket=%d) =====\n", client_sock);
    
    int client_canvas_id = -1;
    
    TCPMessage msg;
    while (1) {
        int bytes = read(client_sock, &msg, sizeof(TCPMessage));
        if (bytes <= 0) {
            printf("[Server][TCP] Client disconnected (socket=%d)\n", client_sock);
            break;
        }

        switch (msg.type) {
            case MSG_LOGIN: {
                int canvas_id = msg.canvas_id;
                char username[32];
                strncpy(username, msg.data, 31);
                username[31] = '\0';
                
                if (canvas_id < 0) canvas_id = 0;
                
                printf("[Server][TCP] LOGIN: user='%s' canvas=%d\n", username, canvas_id);
                
                if (!start_canvas_thread(canvas_id)) {
                    printf("[Server][TCP] ERROR: Failed to start canvas thread\n");
                    break;
                }
                
                client_canvas_id = canvas_id;
                
                CanvasRoom* room = get_or_create_canvas(canvas_id);
                pthread_mutex_lock(&room->mutex);
                room->tcp_clients.push_back(client_sock);
                room->add_user(client_sock, username, nullptr, 0);
                int my_uid = room->users[client_sock]->room_uid;
                
                printf("[Server][TCP] User '%s' registered to canvas #%d (clients: %zu)\n", 
                       username, canvas_id, room->tcp_clients.size());
                pthread_mutex_unlock(&room->mutex);
                
                TCPMessage response;
                memset(&response, 0, sizeof(response));
                response.type = MSG_WELCOME;
                response.canvas_id = canvas_id;
                response.layer_count = room->layers.size();
                response.user_id = my_uid;
                
                write(client_sock, &response, sizeof(TCPMessage));
                printf("[Server][TCP] Sent WELCOME (canvas=%d, layers=%d)\n", canvas_id, response.layer_count);
                
                // Send the actual canvas data to sync the new client
                send_canvas_to_client(client_sock, canvas_id);
                
                // Send existing signatures to the new client
                pthread_mutex_lock(&room->mutex);
                for (auto const& [sock, user] : room->users) {
                    if (sock != client_sock && user->signature_data) {
                        TCPMessage sigMsg;
                        memset(&sigMsg, 0, sizeof(sigMsg));
                        sigMsg.type = MSG_SIGNATURE;
                        sigMsg.canvas_id = canvas_id;
                        sigMsg.data_len = 128;
                        sigMsg.user_id = user->room_uid;
                        memcpy(sigMsg.data, user->signature_data, 128);
                        write(client_sock, &sigMsg, sizeof(TCPMessage));
                        printf("[Server][TCP] Sent existing signature of UID=%d to new client\n", user->room_uid);
                    }
                }
                pthread_mutex_unlock(&room->mutex);
                
                printf("[Server][TCP] User '%s' logged into canvas #%d (UDP port %d)\n", 
                       username, canvas_id, room->udp_port);
                break;
            }

            // --- SIGNATURE IMPLEMENTATION START ---
            case MSG_SIGNATURE: {
                if (client_canvas_id < 0) break;
                
                printf("[Server][TCP] Received SIGNATURE (len=%d)\n", msg.data_len);
                if (msg.data_len == 128) {
                    CanvasRoom* room = get_or_create_canvas(client_canvas_id);
                    pthread_mutex_lock(&room->mutex);
                    
                    // Find user
                    if (room->users.count(client_sock)) {
                        ConnectedUser* u = room->users[client_sock];
                        if (u->signature_data) delete[] u->signature_data;
                        u->signature_len = 128;
                        u->signature_data = new uint8_t[128];
                        memcpy(u->signature_data, msg.data, 128);
                        printf("[Server][TCP] Stored signature for user '%s' (UID=%d)\n", u->username, u->room_uid);
                        
                        // Broadcast to ALL clients in the room (including sender, so they know their ID if needed, 
                        // though client ignores own signature for display)
                        TCPMessage broadcast;
                        memset(&broadcast, 0, sizeof(broadcast));
                        broadcast.type = MSG_SIGNATURE;
                        broadcast.canvas_id = client_canvas_id;
                        broadcast.data_len = 128;
                        broadcast.user_id = u->room_uid; // Send the UID
                        memcpy(broadcast.data, msg.data, 128);
                        
                        broadcast_tcp(room, broadcast);
                    }
                    
                    pthread_mutex_unlock(&room->mutex);
                }
                break;
            }
            // --- SIGNATURE IMPLEMENTATION END ---
            
            case MSG_SAVE:
                printf("[Server][TCP] SAVE request from socket %d\n", client_sock);
                if (client_canvas_id >= 0) {
                    pthread_mutex_lock(&canvases_mutex);
                    save_all_canvases();
                    pthread_mutex_unlock(&canvases_mutex);
                }
                break;
                
            case MSG_LAYER_ADD:
                printf("[Server][TCP] LAYER_ADD request: layer_id=%d\n", msg.layer_id);
                if (client_canvas_id >= 0) {
                    CanvasRoom* room = get_or_create_canvas(client_canvas_id);
                    pthread_mutex_lock(&room->mutex);
                    room->dirty = true;
                    
                    int added_at_index = -1;
                    // Check if it's an insertion or append
                    if (msg.layer_id > 0 && msg.layer_id < room->layers.size()) {
                        room->insert_layer(msg.layer_id);
                        added_at_index = msg.layer_id;
                    } else {
                        room->add_layer();
                        added_at_index = room->layers.size() - 1;
                    }
                    
                    // Prepare and broadcast response
                    TCPMessage response;
                    memset(&response, 0, sizeof(response));
                    response.type = MSG_LAYER_ADD;
                    response.canvas_id = client_canvas_id;
                    response.layer_count = room->layers.size();
                    response.layer_id = added_at_index;
                    
                    broadcast_tcp(room, response);
                    printf("[Server][TCP] Broadcast LAYER_ADD to %zu clients (layers=%d, added_at=%d)\n", 
                           room->tcp_clients.size(), response.layer_count, response.layer_id);
                    
                    pthread_mutex_unlock(&room->mutex);
                }
                break;
                
            case MSG_LAYER_DEL:
                printf("[Server][TCP] LAYER_DEL request: layer=%d\n", msg.layer_id);
                if (client_canvas_id >= 0) {
                    CanvasRoom* room = get_or_create_canvas(client_canvas_id);
                    pthread_mutex_lock(&room->mutex);
                    room->dirty = true;
                    room->delete_layer(msg.layer_id);
                    
                    // Prepare and broadcast response
                    TCPMessage response;
                    memset(&response, 0, sizeof(response));
                    response.type = MSG_LAYER_DEL;
                    response.canvas_id = client_canvas_id;
                    response.layer_count = room->layers.size();
                    response.layer_id = msg.layer_id;
                    
                    broadcast_tcp(room, response);
                    printf("[Server][TCP] Broadcast LAYER_DEL to %zu clients (layers=%d)\n", 
                           room->tcp_clients.size(), response.layer_count);
                    
                    pthread_mutex_unlock(&room->mutex);
                }
                break;
                
            case MSG_LAYER_SYNC:
                printf("[Server][TCP] LAYER_SYNC request: layer=%d\n", msg.layer_id);
                if (client_canvas_id >= 0) {
                    CanvasRoom* room = get_or_create_canvas(client_canvas_id);
                    pthread_mutex_lock(&room->mutex);
                    room->dirty = true;
                    
                    int layer_idx = msg.layer_id;
                    if (layer_idx > 0 && layer_idx < (int)room->layers.size()) {
                        // Receive layer data from client
                        size_t layer_size = WIDTH * HEIGHT * 4;
                        uint8_t* layer_data = new uint8_t[layer_size];
                        size_t received = 0;
                        while (received < layer_size) {
                            ssize_t n = recv(client_sock, layer_data + received, layer_size - received, 0);
                            if (n <= 0) break;
                            received += n;
                        }
                        
                        if (received == layer_size) {
                            // Update server's layer
                            Layer* layer = room->layers[layer_idx];
                            layer->dirty = true;
                            for (int x = 0; x < WIDTH; x++) {
                                for (int y = 0; y < HEIGHT; y++) {
                                    int idx = (y * WIDTH + x) * 4;
                                    layer->pixels[x][y].r = layer_data[idx];
                                    layer->pixels[x][y].g = layer_data[idx + 1];
                                    layer->pixels[x][y].b = layer_data[idx + 2];
                                    layer->pixels[x][y].a = layer_data[idx + 3];
                                }
                            }
                            printf("[Server][TCP] Received layer %d data (%zu bytes)\n", layer_idx, received);
                            
                            // Broadcast to other clients
                            TCPMessage broadcast;
                            memset(&broadcast, 0, sizeof(broadcast));
                            broadcast.type = MSG_LAYER_SYNC;
                            broadcast.canvas_id = client_canvas_id;
                            broadcast.layer_id = layer_idx;
                            broadcast.layer_count = room->layers.size();
                            
                            for (int sock : room->tcp_clients) {
                                if (sock != client_sock) {  // Don't send back to sender
                                    write(sock, &broadcast, sizeof(TCPMessage));
                                    write(sock, layer_data, layer_size);
                                }
                            }
                            printf("[Server][TCP] Broadcast LAYER_SYNC to %zu other clients\n", 
                                   room->tcp_clients.size() - 1);
                        }
                        delete[] layer_data;
                    }
                    
                    pthread_mutex_unlock(&room->mutex);
                }
                break;

            case MSG_LAYER_REORDER:
                // data[0] = old_idx, data[1] = new_idx
                if (client_canvas_id >= 0) {
                    int old_idx = (uint8_t)msg.data[0];
                    int new_idx = (uint8_t)msg.data[1];
                    
                    CanvasRoom* room = get_or_create_canvas(client_canvas_id);
                    pthread_mutex_lock(&room->mutex);
                    room->dirty = true;
                    room->reorder_layer(old_idx, new_idx);
                    
                    // Broadcast
                    TCPMessage resp = msg; // Echo back
                    broadcast_tcp(room, resp);
                    
                    pthread_mutex_unlock(&room->mutex);
                }
                break;

            case MSG_LAYER_MOVE:
                {
                    if (client_canvas_id < 0) break;
                    CanvasRoom* room = get_or_create_canvas(client_canvas_id);
                    
                    // Extract payload
                    struct MoveData { int dx; int dy; } payload;
                    memcpy(&payload, msg.data, sizeof(MoveData));
                    
                    printf("[Server][TCP] LAYER_MOVE: layer=%d dx=%d dy=%d\n", msg.layer_id, payload.dx, payload.dy);

                    pthread_mutex_lock(&room->mutex);
                    room->dirty = true;
                    
                    // 1. Apply to Server's Canvas
                    if (msg.layer_id > 0 && msg.layer_id < (int)room->layers.size()) {
                        move_layer_buffer(room->layers[msg.layer_id], payload.dx, payload.dy);
                    }
                    
                    // 2. Broadcast to others
                    broadcast_tcp(room, msg, client_sock); // Don't send back to sender (they already moved)
                    
                    pthread_mutex_unlock(&room->mutex);
                }
                break;
        }
    }

    if (client_canvas_id >= 0) {
        CanvasRoom* room = get_or_create_canvas(client_canvas_id);
        pthread_mutex_lock(&room->mutex);
        auto& clients = room->tcp_clients;
        clients.erase(remove(clients.begin(), clients.end(), client_sock), clients.end());
        printf("[Server][TCP] Removed socket %d from canvas #%d\n", client_sock, client_canvas_id);
        pthread_mutex_unlock(&room->mutex);
    }
    
    close(client_sock);
    printf("[Server][TCP] ===== Socket %d closed =====\n", client_sock);
    return NULL;
}

/*****************************************************************************
   MAIN
 *****************************************************************************/

int main() {
    printf("============================================\n");
    printf("  Shared Canvas Server v4.0\n");
    printf("  Multi-Layer + On-Demand Canvases\n");
    printf("============================================\n\n");
    
    printf("[Server][Init] On-demand canvas system ready\n");
    
    load_all_canvases();
    
    availableBrushes.push_back(new RoundBrush());
    availableBrushes.push_back(new SquareBrush());
    availableBrushes.push_back(new HardEraserBrush());
    availableBrushes.push_back(new PressureBrush());
    availableBrushes.push_back(new Airbrush());
    availableBrushes.push_back(new RealPaintBrush());
    availableBrushes.push_back(new SoftEraserBrush());
    printf("[Server][Init] Loaded %zu brushes\n", availableBrushes.size());

    printf("[Server][Init] Setting up TCP on port %d...\n", PORT);
    int tcp_sd = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_sd == -1) { perror("[Server] TCP socket"); return 1; }
    
    int on = 1;
    setsockopt(tcp_sd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    
    struct sockaddr_in server_addr;
    bzero(&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(PORT);
    
    if (bind(tcp_sd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        perror("[Server] TCP bind"); return 1;
    }
    if (listen(tcp_sd, 5) == -1) { perror("[Server] TCP listen"); return 1; }

    printf("\n[Server] ===== SERVER READY =====\n");
    printf("[Server] TCP: %d | UDP: %d+ (on-demand) | Layers: %d\n", 
           PORT, PORT+1, MAX_LAYERS);
    printf("==========================================\n\n");

    pthread_t save_th;
    pthread_create(&save_th, NULL, autosave_thread, NULL);

    printf("[Server] Waiting for connections...\n\n");
    while (1) {
        struct sockaddr_in from;
        socklen_t length = sizeof(from);
        
        int client = accept(tcp_sd, (struct sockaddr*)&from, &length);
        if (client < 0) { perror("[Server] Accept"); continue; }
        
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &from.sin_addr, client_ip, INET_ADDRSTRLEN);
        printf("[Server] New connection: %s:%d (socket=%d)\n", client_ip, ntohs(from.sin_port), client);

        int* arg = (int*)malloc(sizeof(int));
        *arg = client;
        
        pthread_t tcp_th;
        pthread_create(&tcp_th, NULL, tcp_client_session, arg);
    }
}
