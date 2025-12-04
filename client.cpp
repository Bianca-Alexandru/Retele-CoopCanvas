/*
   Shared Canvas Client
   
   Multi-layer drawing client with SDL2 UI.
   Connects to server via TCP for control and UDP for canvas updates.
   
   Features:
   - Multi-canvas lobby system
   - Multi-layer support (layer 0 = white paper, layer 1+ = transparent)
   - Multiple brush types
   - Color picker with hue/saturation
   - Real-time synchronization
*/

#include <SDL2/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

using namespace std;

#include "brushes.h"

/* ============================================================================
   CONSTANTS AND TYPES
   ============================================================================ */

#define CANVAS_WIDTH  640
#define CANVAS_HEIGHT 480
#define TCP_PORT      6769
#define UDP_BASE_PORT 6770

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
    MSG_LAYER_SYNC = 13  // Full layer data sync (for undo/redo)
};

struct TCPMessage {
    uint8_t  type;
    uint8_t  canvas_id;
    uint16_t data_len;
    uint8_t  layer_count;
    uint8_t  layer_id;
    char     data[256];
} __attribute__((packed));

struct UDPMessage {
    uint8_t  type;
    uint8_t  brush_id;
    uint8_t  layer_id;
    int16_t  x;
    int16_t  y;
    int16_t  ex, ey;  // for line drawing
    uint8_t  r, g, b, a;
    uint8_t  size;
} __attribute__((packed));

/* ============================================================================
   GLOBAL STATE
   ============================================================================ */

// Connection
int tcpSock = -1;
int udpSock = -1;
struct sockaddr_in serverTcpAddr, serverUdpAddr;
char serverIp[64] = "127.0.0.1";

// Canvas state
int currentCanvasId = 0;
int currentLayerId = 1;  // Start on layer 1 (not layer 0 which is paper)
int layerCount = 2;      // Layer 0 (paper) + Layer 1 (drawable)
int loggedin = 0;
int running = 1;

// Drawing
SDL_Color userColor = {0, 0, 0, 255};
int currentBrushId = 0;
int mouseDown = 0;
int lastMouseX = -1, lastMouseY = -1;

// Layer system - each layer is CANVAS_WIDTH * CANVAS_HEIGHT * 4 bytes (RGBA)
#define MAX_LAYERS 10
#define MAX_UNDO_HISTORY 20
uint8_t* layers[MAX_LAYERS] = {nullptr};  // Layer 0 = paper (white), Layer 1+ = drawable
uint8_t* compositeCanvas = nullptr;       // Final composited image for display
pthread_mutex_t layerMutex = PTHREAD_MUTEX_INITIALIZER;  // Protects layers array and layerCount

// Flag for pending UI updates (set by TCP thread, handled by main thread)
volatile bool pendingLayerUpdate = false;

// Undo/Redo system - stores snapshots of layers
struct CanvasSnapshot {
    uint8_t* data[MAX_LAYERS];
    int layerCount;
    
    CanvasSnapshot() {
        for (int i = 0; i < MAX_LAYERS; i++) data[i] = nullptr;
        layerCount = 0;
    }
    
    ~CanvasSnapshot() {
        for (int i = 0; i < MAX_LAYERS; i++) {
            if (data[i]) delete[] data[i];
        }
    }
    
    void capture() {
        for (int i = 0; i < MAX_LAYERS; i++) {
            if (data[i]) { delete[] data[i]; data[i] = nullptr; }
            if (layers[i]) {
                data[i] = new uint8_t[CANVAS_WIDTH * CANVAS_HEIGHT * 4];
                memcpy(data[i], layers[i], CANVAS_WIDTH * CANVAS_HEIGHT * 4);
            }
        }
        layerCount = ::layerCount;
    }
    
    void restore() {
        for (int i = 0; i < MAX_LAYERS; i++) {
            if (data[i] && layers[i]) {
                memcpy(layers[i], data[i], CANVAS_WIDTH * CANVAS_HEIGHT * 4);
            }
        }
        ::layerCount = layerCount;
    }
};

vector<CanvasSnapshot*> undoStack;
vector<CanvasSnapshot*> redoStack;
bool strokeInProgress = false;

// Brush system
vector<Brush*> availableBrushes;

// Remote cursor tracking (forward declaration, defined in ui.h)
struct RemoteCursor;
map<string, RemoteCursor> remote_cursors;

// SDL
SDL_Window* window = nullptr;
SDL_Renderer* renderer = nullptr;
SDL_Texture* canvasTexture = nullptr;

// UI system
#include "ui.h"
vector<Button*> buttons;

/* ============================================================================
   FORWARD DECLARATIONS
   ============================================================================ */

void send_tcp_login(const char* username);
void send_tcp_save();
void send_tcp_add_layer();
void send_tcp_delete_layer(int layer_id);
void send_tcp_layer_sync(int layer_id);
void send_all_layers_sync();
void send_udp_draw(int x, int y);
void* tcp_receiver_thread(void* arg);
void* udp_receiver_thread(void* arg);
void init_layer(int layer_idx, bool white);
void save_undo_state();
void perform_undo();
void perform_redo();

// TCP thread handle
pthread_t tcp_receiver_tid = 0;

/* ============================================================================
   NETWORK FUNCTIONS
   ============================================================================ */

int connect_tcp() {
    printf("[Client][TCP] Connecting to %s:%d...\n", serverIp, TCP_PORT);
    
    tcpSock = socket(AF_INET, SOCK_STREAM, 0);
    if (tcpSock < 0) {
        perror("[Client][TCP] Socket creation failed");
        return -1;
    }

    memset(&serverTcpAddr, 0, sizeof(serverTcpAddr));
    serverTcpAddr.sin_family = AF_INET;
    serverTcpAddr.sin_port = htons(TCP_PORT);
    inet_pton(AF_INET, serverIp, &serverTcpAddr.sin_addr);

    if (connect(tcpSock, (struct sockaddr*)&serverTcpAddr, sizeof(serverTcpAddr)) < 0) {
        perror("[Client][TCP] Connection failed");
        close(tcpSock);
        tcpSock = -1;
        return -1;
    }

    printf("[Client][TCP] Connected successfully!\n");
    return 0;
}

int setup_udp(int canvas_id) {
    printf("[Client][UDP] Setting up socket for canvas #%d (port %d)...\n", canvas_id, UDP_BASE_PORT + canvas_id);
    
    udpSock = socket(AF_INET, SOCK_DGRAM, 0);
    if (udpSock < 0) {
        perror("[Client][UDP] Socket creation failed");
        return -1;
    }

    memset(&serverUdpAddr, 0, sizeof(serverUdpAddr));
    serverUdpAddr.sin_family = AF_INET;
    serverUdpAddr.sin_port = htons(UDP_BASE_PORT + canvas_id);
    inet_pton(AF_INET, serverIp, &serverUdpAddr.sin_addr);

    printf("[Client][UDP] Socket ready for canvas #%d\n", canvas_id);
    return 0;
}

void send_tcp_login(const char* username) {
    if (tcpSock < 0) {
        if (connect_tcp() < 0) {
            printf("[Client][TCP] Failed to connect for login!\n");
            return;
        }
        // Start TCP receiver thread after connecting
        if (tcp_receiver_tid == 0) {
            printf("[Client][TCP] Starting receiver thread...\n");
            pthread_create(&tcp_receiver_tid, NULL, tcp_receiver_thread, NULL);
            pthread_detach(tcp_receiver_tid);
        }
    }

    printf("[Client][TCP] Sending login request: canvas=%d, user=%s\n", currentCanvasId, username);
    
    TCPMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_LOGIN;
    msg.canvas_id = currentCanvasId;
    strncpy(msg.data, username, sizeof(msg.data) - 1);
    msg.data_len = strlen(username);

    if (send(tcpSock, &msg, sizeof(msg), 0) < 0) {
        perror("[Client][TCP] Login send failed");
        return;
    }
    
    printf("[Client][TCP] Login request sent\n");
}

void send_tcp_save() {
    if (tcpSock < 0) return;
    
    printf("[Client][TCP] Sending save request...\n");
    
    TCPMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_SAVE;
    msg.canvas_id = currentCanvasId;
    msg.data_len = 0;

    if (send(tcpSock, &msg, sizeof(msg), 0) < 0) {
        perror("[Client][TCP] Save send failed");
    } else {
        printf("[Client][TCP] Save request sent\n");
    }
}

void send_tcp_add_layer() {
    if (tcpSock < 0) return;
    
    printf("[Client][TCP] Sending add layer request...\n");
    
    TCPMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_LAYER_ADD;
    msg.canvas_id = currentCanvasId;
    msg.data_len = 0;

    if (send(tcpSock, &msg, sizeof(msg), 0) < 0) {
        perror("[Client][TCP] Add layer send failed");
    } else {
        printf("[Client][TCP] Add layer request sent\n");
    }
}

void send_tcp_delete_layer(int layer_id) {
    if (tcpSock < 0) return;
    
    printf("[Client][TCP] Sending delete layer request: layer=%d\n", layer_id);
    
    TCPMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_LAYER_DEL;
    msg.canvas_id = currentCanvasId;
    msg.layer_id = layer_id;
    msg.data_len = 0;

    if (send(tcpSock, &msg, sizeof(msg), 0) < 0) {
        perror("[Client][TCP] Delete layer send failed");
    } else {
        printf("[Client][TCP] Delete layer request sent\n");
    }
}

void send_tcp_layer_sync(int layer_id) {
    if (tcpSock < 0) return;
    if (layer_id <= 0 || layer_id >= MAX_LAYERS || !layers[layer_id]) return;
    
    printf("[Client][TCP] Sending layer sync: layer=%d\n", layer_id);
    
    TCPMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_LAYER_SYNC;
    msg.canvas_id = currentCanvasId;
    msg.layer_id = layer_id;
    msg.data_len = 0;

    if (send(tcpSock, &msg, sizeof(msg), 0) < 0) {
        perror("[Client][TCP] Layer sync header send failed");
        return;
    }
    
    // Send full layer data
    size_t layer_size = CANVAS_WIDTH * CANVAS_HEIGHT * 4;
    if (send(tcpSock, layers[layer_id], layer_size, 0) < 0) {
        perror("[Client][TCP] Layer sync data send failed");
        return;
    }
    
    printf("[Client][TCP] Layer sync sent (%zu bytes)\n", layer_size);
}

void send_all_layers_sync() {
    // Send all drawable layers to server for sync
    for (int l = 1; l < layerCount && l < MAX_LAYERS; l++) {
        if (layers[l]) {
            send_tcp_layer_sync(l);
        }
    }
}

void send_udp_draw(int x, int y) {
    if (udpSock < 0) return;
    if (x < 0 || x >= CANVAS_WIDTH || y < 0 || y >= CANVAS_HEIGHT) return;
    if (currentLayerId <= 0) {
        printf("[Client][Draw] Cannot draw on layer 0 (paper)!\n");
        return;
    }
    if (currentLayerId >= MAX_LAYERS || !layers[currentLayerId]) {
        printf("[Client][Draw] Invalid layer %d!\n", currentLayerId);
        return;
    }

    UDPMessage pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = MSG_DRAW;
    pkt.brush_id = currentBrushId;
    pkt.layer_id = currentLayerId;
    pkt.x = x;
    pkt.y = y;
    pkt.r = userColor.r;
    pkt.g = userColor.g;
    pkt.b = userColor.b;
    pkt.a = userColor.a;
    pkt.size = (currentBrushId < (int)availableBrushes.size()) ? availableBrushes[currentBrushId]->size : 5;

    sendto(udpSock, &pkt, sizeof(pkt), 0, (struct sockaddr*)&serverUdpAddr, sizeof(serverUdpAddr));
    
    // Also apply locally to the correct layer for immediate feedback
    if (currentBrushId < (int)availableBrushes.size()) {
        SDL_Color col = userColor;
        int layer_idx = currentLayerId;
        availableBrushes[currentBrushId]->paint(x, y, col, availableBrushes[currentBrushId]->size,
            [layer_idx](int px, int py, SDL_Color c) {
                if (px >= 0 && px < CANVAS_WIDTH && py >= 0 && py < CANVAS_HEIGHT) {
                    int idx = (py * CANVAS_WIDTH + px) * 4;
                    layers[layer_idx][idx]     = c.r;
                    layers[layer_idx][idx + 1] = c.g;
                    layers[layer_idx][idx + 2] = c.b;
                    layers[layer_idx][idx + 3] = c.a;
                }
            });
    }
}

void send_udp_cursor(int x, int y) {
    if (udpSock < 0) return;

    UDPMessage pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = MSG_CURSOR;
    pkt.x = x;
    pkt.y = y;
    pkt.r = userColor.r;
    pkt.g = userColor.g;
    pkt.b = userColor.b;
    pkt.a = 255;

    sendto(udpSock, &pkt, sizeof(pkt), 0, (struct sockaddr*)&serverUdpAddr, sizeof(serverUdpAddr));
}

/* ============================================================================
   THREAD FUNCTIONS
   ============================================================================ */

void* tcp_receiver_thread(void* arg) {
    (void)arg;
    printf("[Client][TCP-Thread] Started receiver thread\n");
    
    TCPMessage msg;
    while (running) {
        memset(&msg, 0, sizeof(msg));
        ssize_t n = recv(tcpSock, &msg, sizeof(msg), 0);
        if (n <= 0) {
            if (running) {
                printf("[Client][TCP-Thread] Connection closed or error\n");
            }
            break;
        }

        printf("[Client][TCP-Thread] Received message: type=%d, canvas=%d, data_len=%d\n",
               msg.type, msg.canvas_id, msg.data_len);

        switch (msg.type) {
            case MSG_WELCOME:
                printf("[Client][TCP-Thread] WELCOME received! Canvas #%d, layers=%d\n", 
                       msg.canvas_id, msg.layer_count);
                loggedin = 1;
                layerCount = msg.layer_count > 0 ? msg.layer_count : 2;
                currentLayerId = 1;
                
                // Initialize all layers based on layer_count
                for (int l = 1; l < layerCount && l < MAX_LAYERS; l++) {
                    if (!layers[l]) {
                        init_layer(l, false);  // transparent
                        printf("[Client][TCP-Thread] Created layer %d\n", l);
                    }
                }
                
                // Receive layer data from server (skip layer 0 which is white paper)
                {
                    int recv_layer_count;
                    if (recv(tcpSock, &recv_layer_count, sizeof(int), MSG_WAITALL) == sizeof(int)) {
                        printf("[Client][TCP-Thread] Receiving %d layers from server\n", recv_layer_count);
                        
                        for (int l = 1; l < recv_layer_count; l++) {
                            if (!layers[l]) {
                                init_layer(l, false);
                            }
                            
                            size_t layer_size = CANVAS_WIDTH * CANVAS_HEIGHT * 4;
                            size_t received = 0;
                            uint8_t* ptr = layers[l];
                            
                            while (received < layer_size) {
                                ssize_t r = recv(tcpSock, ptr + received, layer_size - received, 0);
                                if (r <= 0) break;
                                received += r;
                            }
                            
                            printf("[Client][TCP-Thread] Received layer %d: %zu bytes\n", l, received);
                        }
                    }
                }
                
                // Setup UDP for this canvas
                if (setup_udp(currentCanvasId) < 0) {
                    printf("[Client][TCP-Thread] UDP setup failed!\n");
                } else {
                    pthread_t udp_tid;
                    pthread_create(&udp_tid, NULL, udp_receiver_thread, NULL);
                    pthread_detach(udp_tid);
                }
                UpdateLayerButtons();
                break;

            case MSG_CANVAS_DATA:
                printf("[Client][TCP-Thread] CANVAS_DATA received: %d bytes\n", msg.data_len);
                break;

            case MSG_LAYER_ADD:
                printf("[Client][TCP-Thread] LAYER_ADD confirmed: new layer count=%d\n", msg.layer_count);
                pthread_mutex_lock(&layerMutex);
                layerCount = msg.layer_count;
                // Create the new layer locally
                if (layerCount > 1 && layerCount <= MAX_LAYERS && !layers[layerCount - 1]) {
                    init_layer(layerCount - 1, false);
                    printf("[Client][TCP-Thread] Created layer %d locally\n", layerCount - 1);
                }
                pendingLayerUpdate = true;  // Main thread will call UpdateLayerButtons()
                pthread_mutex_unlock(&layerMutex);
                break;

            case MSG_LAYER_DEL:
                printf("[Client][TCP-Thread] LAYER_DEL confirmed: deleted layer %d, new count=%d\n", 
                       msg.layer_id, msg.layer_count);
                pthread_mutex_lock(&layerMutex);
                // Delete local layer and shift remaining layers down
                if (msg.layer_id > 0 && msg.layer_id < MAX_LAYERS) {
                    // Delete the layer
                    if (layers[msg.layer_id]) {
                        delete[] layers[msg.layer_id];
                        layers[msg.layer_id] = nullptr;
                    }
                    // Shift all layers above down by one
                    for (int l = msg.layer_id; l < MAX_LAYERS - 1; l++) {
                        layers[l] = layers[l + 1];
                    }
                    layers[MAX_LAYERS - 1] = nullptr;
                    printf("[Client][TCP-Thread] Shifted layers down after deleting layer %d\n", msg.layer_id);
                }
                layerCount = msg.layer_count;
                if (currentLayerId >= layerCount) currentLayerId = layerCount - 1;
                if (currentLayerId < 1) currentLayerId = 1;
                pendingLayerUpdate = true;  // Main thread will call UpdateLayerButtons()
                pthread_mutex_unlock(&layerMutex);
                break;

            case MSG_LAYER_SYNC:
                printf("[Client][TCP-Thread] LAYER_SYNC received: layer=%d\n", msg.layer_id);
                {
                    int layer_idx = msg.layer_id;
                    if (layer_idx > 0 && layer_idx < MAX_LAYERS) {
                        pthread_mutex_lock(&layerMutex);
                        
                        // Ensure layer exists
                        if (!layers[layer_idx]) {
                            init_layer(layer_idx, false);
                        }
                        
                        // Receive layer data
                        size_t layer_size = CANVAS_WIDTH * CANVAS_HEIGHT * 4;
                        size_t received = 0;
                        uint8_t* ptr = layers[layer_idx];
                        while (received < layer_size) {
                            ssize_t n = recv(tcpSock, ptr + received, layer_size - received, 0);
                            if (n <= 0) break;
                            received += n;
                        }
                        
                        if (received == layer_size) {
                            printf("[Client][TCP-Thread] Layer %d synced (%zu bytes)\n", layer_idx, received);
                        } else {
                            printf("[Client][TCP-Thread] Layer sync incomplete: %zu/%zu bytes\n", received, layer_size);
                        }
                        
                        pthread_mutex_unlock(&layerMutex);
                    }
                }
                break;

            case MSG_ERROR:
                printf("[Client][TCP-Thread] ERROR from server: %s\n", msg.data);
                break;

            default:
                printf("[Client][TCP-Thread] Unknown message type: %d\n", msg.type);
                break;
        }
    }

    printf("[Client][TCP-Thread] Exiting\n");
    return NULL;
}

void* udp_receiver_thread(void* arg) {
    (void)arg;
    printf("[Client][UDP-Thread] Started receiver thread for canvas #%d\n", currentCanvasId);

    struct sockaddr_in fromAddr;
    socklen_t fromLen = sizeof(fromAddr);
    uint8_t buffer[2048];

    while (running && loggedin) {
        ssize_t n = recvfrom(udpSock, buffer, sizeof(buffer), 0, (struct sockaddr*)&fromAddr, &fromLen);
        if (n <= 0) continue;

        if (n >= (ssize_t)sizeof(UDPMessage)) {
            UDPMessage* pkt = (UDPMessage*)buffer;
            
            switch (pkt->type) {
                case MSG_DRAW:
                    {
                        int layer_idx = pkt->layer_id;
                        if (layer_idx <= 0 || layer_idx >= MAX_LAYERS) layer_idx = 1;
                        
                        // Ensure layer exists
                        if (!layers[layer_idx]) {
                            init_layer(layer_idx, false);
                        }
                        
                        // Apply to the correct layer (no logging - too spammy)
                        if (pkt->brush_id < (int)availableBrushes.size()) {
                            SDL_Color col = {pkt->r, pkt->g, pkt->b, pkt->a};
                            int brushSize = pkt->size > 0 ? pkt->size : 5;
                            availableBrushes[pkt->brush_id]->paint(pkt->x, pkt->y, col, brushSize,
                                [layer_idx](int px, int py, SDL_Color c) {
                                    if (px >= 0 && px < CANVAS_WIDTH && py >= 0 && py < CANVAS_HEIGHT) {
                                        int idx = (py * CANVAS_WIDTH + px) * 4;
                                        layers[layer_idx][idx]     = c.r;
                                        layers[layer_idx][idx + 1] = c.g;
                                        layers[layer_idx][idx + 2] = c.b;
                                        layers[layer_idx][idx + 3] = c.a;
                                    }
                                });
                        }
                    }
                    break;

                case MSG_CURSOR:
                    // Remote cursor
                    {
                        char key[32];
                        snprintf(key, sizeof(key), "%s:%d", 
                                 inet_ntoa(fromAddr.sin_addr), ntohs(fromAddr.sin_port));
                        remote_cursors[key] = {pkt->x, pkt->y, {pkt->r, pkt->g, pkt->b, 255}};
                    }
                    break;

                default:
                    break;
            }
        }
    }

    printf("[Client][UDP-Thread] Exiting\n");
    return NULL;
}

/* ============================================================================
   DRAWING FUNCTIONS
   ============================================================================ */

void init_layer(int layer_idx, bool white) {
    if (layer_idx < 0 || layer_idx >= MAX_LAYERS) return;
    
    if (layers[layer_idx]) {
        delete[] layers[layer_idx];
    }
    layers[layer_idx] = new uint8_t[CANVAS_WIDTH * CANVAS_HEIGHT * 4];
    
    if (white) {
        // White opaque (paper)
        for (int i = 0; i < CANVAS_WIDTH * CANVAS_HEIGHT * 4; i += 4) {
            layers[layer_idx][i]     = 255;
            layers[layer_idx][i + 1] = 255;
            layers[layer_idx][i + 2] = 255;
            layers[layer_idx][i + 3] = 255;
        }
    } else {
        // Fully transparent
        memset(layers[layer_idx], 0, CANVAS_WIDTH * CANVAS_HEIGHT * 4);
    }
}

void save_undo_state() {
    // Clear redo stack when new action is performed
    for (auto* snap : redoStack) delete snap;
    redoStack.clear();
    
    // Save current state to undo stack
    CanvasSnapshot* snapshot = new CanvasSnapshot();
    snapshot->capture();
    undoStack.push_back(snapshot);
    
    // Limit undo history size
    while (undoStack.size() > MAX_UNDO_HISTORY) {
        delete undoStack.front();
        undoStack.erase(undoStack.begin());
    }
}

void perform_undo() {
    if (undoStack.empty()) {
        printf("[Client][Undo] Nothing to undo\n");
        return;
    }
    
    // Save current state to redo stack
    CanvasSnapshot* redoSnap = new CanvasSnapshot();
    redoSnap->capture();
    redoStack.push_back(redoSnap);
    
    // Restore from undo stack
    CanvasSnapshot* undoSnap = undoStack.back();
    undoStack.pop_back();
    undoSnap->restore();
    delete undoSnap;
    
    // Sync changes to other clients
    send_all_layers_sync();
    
    printf("[Client][Undo] Undo performed (undo stack: %zu, redo stack: %zu)\n", 
           undoStack.size(), redoStack.size());
}

void perform_redo() {
    if (redoStack.empty()) {
        printf("[Client][Redo] Nothing to redo\n");
        return;
    }
    
    // Save current state to undo stack
    CanvasSnapshot* undoSnap = new CanvasSnapshot();
    undoSnap->capture();
    undoStack.push_back(undoSnap);
    
    // Restore from redo stack
    CanvasSnapshot* redoSnap = redoStack.back();
    redoStack.pop_back();
    redoSnap->restore();
    delete redoSnap;
    
    // Sync changes to other clients
    send_all_layers_sync();
    
    printf("[Client][Redo] Redo performed (undo stack: %zu, redo stack: %zu)\n", 
           undoStack.size(), redoStack.size());
}

void init_canvas() {
    printf("[Client][Canvas] Initializing layer system (%dx%d)...\n", CANVAS_WIDTH, CANVAS_HEIGHT);
    
    // Initialize composite buffer
    if (compositeCanvas) delete[] compositeCanvas;
    compositeCanvas = new uint8_t[CANVAS_WIDTH * CANVAS_HEIGHT * 4];
    
    // Initialize layer 0 (white paper)
    init_layer(0, true);
    printf("[Client][Canvas] Layer 0 (paper) initialized to white\n");
    
    // Initialize layer 1 (first drawable, transparent)
    init_layer(1, false);
    printf("[Client][Canvas] Layer 1 initialized as transparent\n");
    
    printf("[Client][Canvas] Canvas initialized with %d layers\n", layerCount);
}

void composite_layers() {
    pthread_mutex_lock(&layerMutex);
    
    // Start with layer 0 (white paper)
    memcpy(compositeCanvas, layers[0], CANVAS_WIDTH * CANVAS_HEIGHT * 4);
    
    // Composite each layer on top using alpha blending
    for (int l = 1; l < layerCount && l < MAX_LAYERS; l++) {
        if (!layers[l]) continue;
        
        for (int i = 0; i < CANVAS_WIDTH * CANVAS_HEIGHT * 4; i += 4) {
            uint8_t srcR = layers[l][i];
            uint8_t srcG = layers[l][i + 1];
            uint8_t srcB = layers[l][i + 2];
            uint8_t srcA = layers[l][i + 3];
            
            if (srcA == 0) continue;  // Fully transparent, skip
            
            uint8_t dstR = compositeCanvas[i];
            uint8_t dstG = compositeCanvas[i + 1];
            uint8_t dstB = compositeCanvas[i + 2];
            
            if (srcA == 255) {
                // Fully opaque, just copy
                compositeCanvas[i]     = srcR;
                compositeCanvas[i + 1] = srcG;
                compositeCanvas[i + 2] = srcB;
                compositeCanvas[i + 3] = 255;
            } else {
                // Alpha blend
                float alpha = srcA / 255.0f;
                compositeCanvas[i]     = (uint8_t)(srcR * alpha + dstR * (1 - alpha));
                compositeCanvas[i + 1] = (uint8_t)(srcG * alpha + dstG * (1 - alpha));
                compositeCanvas[i + 2] = (uint8_t)(srcB * alpha + dstB * (1 - alpha));
                compositeCanvas[i + 3] = 255;
            }
        }
    }
    
    pthread_mutex_unlock(&layerMutex);
}

void update_canvas_texture() {
    if (!canvasTexture || !compositeCanvas) return;
    
    composite_layers();
    SDL_UpdateTexture(canvasTexture, NULL, compositeCanvas, CANVAS_WIDTH * 4);
}

/* ============================================================================
   BRUSH INITIALIZATION
   ============================================================================ */

void init_brushes() {
    printf("[Client][Brushes] Initializing brush system...\n");
    
    availableBrushes.push_back(new RoundBrush());
    availableBrushes.push_back(new SquareBrush());
    availableBrushes.push_back(new HardEraserBrush());
    
    for (int i = 0; i < (int)availableBrushes.size(); i++) {
        availableBrushes[i]->size = 5;
    }
    
    printf("[Client][Brushes] %zu brushes loaded\n", availableBrushes.size());
}

/* ============================================================================
   SDL EVENT HANDLING
   ============================================================================ */

void handle_events() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_QUIT:
                printf("[Client][Event] QUIT received\n");
                running = 0;
                break;

            case SDL_MOUSEBUTTONDOWN:
                if (e.button.button == SDL_BUTTON_LEFT) {
                    int mx = e.button.x;
                    int my = e.button.y;
                    
                    if (!loggedin) {
                        handle_login_screen_click(mx, my);
                    } else {
                        // Check UI first
                        if (!handle_canvas_ui_click(mx, my)) {
                            // Drawing on canvas - save undo state before starting stroke
                            if (!strokeInProgress) {
                                save_undo_state();
                                strokeInProgress = true;
                            }
                            mouseDown = 1;
                            lastMouseX = mx;
                            lastMouseY = my;
                            send_udp_draw(mx, my);
                        }
                    }
                }
                break;

            case SDL_MOUSEBUTTONUP:
                if (e.button.button == SDL_BUTTON_LEFT) {
                    if (mouseDown) {
                        strokeInProgress = false;
                    }
                    mouseDown = 0;
                    lastMouseX = -1;
                    lastMouseY = -1;
                }
                break;

            case SDL_MOUSEMOTION:
                if (loggedin) {
                    int mx = e.motion.x;
                    int my = e.motion.y;
                    
                    send_udp_cursor(mx, my);
                    
                    if (mouseDown) {
                        // Interpolate between last position and current
                        if (lastMouseX >= 0 && lastMouseY >= 0) {
                            int dx = mx - lastMouseX;
                            int dy = my - lastMouseY;
                            int steps = max(abs(dx), abs(dy));
                            if (steps > 0) {
                                for (int i = 1; i <= steps; i++) {
                                    int ix = lastMouseX + (dx * i) / steps;
                                    int iy = lastMouseY + (dy * i) / steps;
                                    send_udp_draw(ix, iy);
                                }
                            }
                        }
                        lastMouseX = mx;
                        lastMouseY = my;
                    }
                }
                break;

            case SDL_KEYDOWN:
                switch (e.key.keysym.sym) {
                    case SDLK_ESCAPE:
                        running = 0;
                        break;
                    case SDLK_s:
                        if (e.key.keysym.mod & KMOD_CTRL) {
                            send_tcp_save();
                        }
                        break;
                    case SDLK_z:
                        if (e.key.keysym.mod & KMOD_CTRL) {
                            perform_undo();
                        }
                        break;
                    case SDLK_y:
                        if (e.key.keysym.mod & KMOD_CTRL) {
                            perform_redo();
                        }
                        break;
                    case SDLK_1:
                        currentBrushId = 0;
                        break;
                    case SDLK_2:
                        currentBrushId = 1;
                        break;
                    case SDLK_3:
                        currentBrushId = 2;
                        break;
                    case SDLK_LEFTBRACKET:
                        if (currentLayerId > 1) {
                            currentLayerId--;
                        }
                        break;
                    case SDLK_RIGHTBRACKET:
                        if (currentLayerId < layerCount - 1) {
                            currentLayerId++;
                        }
                        break;
                    default:
                        break;
                }
                break;

            default:
                break;
        }
    }
}

/* ============================================================================
   MAIN
   ============================================================================ */

int main(int argc, char* argv[]) {
    printf("[Client][Main] ==============================================\n");
    printf("[Client][Main] Shared Canvas Client Starting\n");
    printf("[Client][Main] ==============================================\n");
    
    // Parse command line
    if (argc > 1) {
        strncpy(serverIp, argv[1], sizeof(serverIp) - 1);
    }
    printf("[Client][Main] Server IP: %s\n", serverIp);

    // Initialize SDL
    printf("[Client][Main] Initializing SDL...\n");
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        printf("[Client][Main] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    window = SDL_CreateWindow("Shared Canvas",
                              SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              CANVAS_WIDTH, CANVAS_HEIGHT,
                              SDL_WINDOW_SHOWN);
    if (!window) {
        printf("[Client][Main] SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) {
        printf("[Client][Main] SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    canvasTexture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                      SDL_TEXTUREACCESS_STREAMING,
                                      CANVAS_WIDTH, CANVAS_HEIGHT);
    if (!canvasTexture) {
        printf("[Client][Main] SDL_CreateTexture failed: %s\n", SDL_GetError());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    printf("[Client][Main] SDL initialized successfully\n");

    // Initialize systems
    init_canvas();
    init_brushes();
    SetupUI();

    printf("[Client][Main] Starting main loop...\n");
    printf("[Client][Main] Use arrow keys in lobby to select canvas (0-9)\n");
    printf("[Client][Main] Click Login button to join a canvas\n");
    printf("[Client][Main] Press [ or ] to switch layers\n");
    printf("[Client][Main] Press 1-3 to switch brushes\n");
    printf("[Client][Main] Press Ctrl+S to save\n");
    printf("[Client][Main] Press ESC to quit\n");
    
    // Main loop
    while (running) {
        handle_events();
        
        // Check for pending layer updates from TCP thread
        if (pendingLayerUpdate) {
            pendingLayerUpdate = false;
            UpdateLayerButtons();
        }

        update_canvas_texture();
        draw_ui(renderer);
        
        SDL_Delay(16); // ~60 FPS
    }

    printf("[Client][Main] Shutting down...\n");

    // Cleanup
    if (tcpSock >= 0) close(tcpSock);
    if (udpSock >= 0) close(udpSock);
    
    for (auto* brush : availableBrushes) delete brush;
    for (auto* btn : buttons) delete btn;
    
    // Cleanup undo/redo stacks
    for (auto* snap : undoStack) delete snap;
    for (auto* snap : redoStack) delete snap;
    undoStack.clear();
    redoStack.clear();
    
    // Cleanup layers
    for (int i = 0; i < MAX_LAYERS; i++) {
        if (layers[i]) delete[] layers[i];
    }
    if (compositeCanvas) delete[] compositeCanvas;
    
    SDL_DestroyTexture(canvasTexture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    printf("[Client][Main] Goodbye!\n");
    return 0;
}
