/*
   COOP CANVAS CLIENT TLDR
   
   -Thread for tcp and udp listening
    -Main thread for UI and drawing
    -Uses ui from ui.h and brushes from brushes.h
    -Doesn't yet implement sdl text, everything is manually drawn for now
    -TCP port at 6769 UDP port at 6770 + canvasID
*/

#include <SDL2/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>
#include <string>
#include <vector>
#include <map>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <algorithm>
#include <csignal>

using namespace std;

#include "brushes.h"
#include "RawInput.h"
#include "undo.h"

/*****************************************************************************
   CONSTANTS AND TYPES
   *****************************************************************************/

#define MENU_WIDTH    640
#define MENU_HEIGHT   480
#define CANVAS_WIDTH  1280
#define CANVAS_HEIGHT 720
#define TCP_PORT      6769
#define UDP_BASE_PORT 6770

const int BRUSH_ROUND_ID = 0;
const int BRUSH_SQUARE_ID = 1;
const int BRUSH_ERASER_ID = 2;
const int BRUSH_SOFT_ERASER_ID = 3;
const int BRUSH_PRESSURE_ID = 4;
const int BRUSH_AIRBRUSH_ID = 5;
const int BRUSH_TEXTURE_ID = 6;

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
    MSG_LAYER_REORDER = 14,
    MSG_SIGNATURE = 15,     // New signature message
    MSG_LAYER_MOVE = 17
};

#define SIGNATURE_WIDTH 450
#define SIGNATURE_HEIGHT 150
#define MAX_SIGNATURE_SIZE (SIGNATURE_WIDTH * SIGNATURE_HEIGHT) // 1 byte per pixel (alpha)

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

struct UDPMessage {
    uint8_t  type;
    uint8_t  brush_id;
    uint8_t  layer_id;
    int16_t  x;
    int16_t  y;
    int16_t  ex, ey;  // for line drawing
    uint8_t  r, g, b, a;
    uint8_t  size;
    uint8_t  pressure;  // 0-255 representing 0.0-1.0 pressure 
} __attribute__((packed));

/*****************************************************************************
   GLOBAL STATE
 *****************************************************************************/

// Connection
int tcpSock = -1;
int udpSock = -1;
struct sockaddr_in serverTcpAddr, serverUdpAddr;
char serverIp[64] = "127.0.0.1";
bool use_raw_input = false;
bool uiVisible = true; // Default to visible

// Window Dimensions (Dynamic)
int windowWidth = MENU_WIDTH;
int windowHeight = MENU_HEIGHT;

// Drawing
SDL_Color userColor = {0, 0, 0, 255};
int currentBrushId = 0;
int mouseDown = 0;
int lastMouseX = -1, lastMouseY = -1;
int lastStableAngle = 0; // Keeps track of the last valid drawing angle
int lastSentPressure = -1; // Track last sent pressure to detect changes
bool isEyedropping = false; // New state for eyedropper tool

// Undo Timeout
uint32_t lastActionTime = 0;
const uint32_t UNDO_TIMEOUT_MS = 100000; // 100 seconds for now (was 15000)

// Viewport / Panning
int viewOffsetX = 0;
int viewOffsetY = 0;
bool isPanning = false;
bool spacePressed = false;

// Signature / Login
SDL_Texture* signatureTexture = nullptr;
SDL_Rect signatureRect = {0, 0, SIGNATURE_WIDTH, SIGNATURE_HEIGHT}; // Will be centered in UI
bool isDrawingSignature = false;
int lastSigX = -1, lastSigY = -1;

// Remote Signature (Echoed back)
// SDL_Texture* remoteSignatureTexture = nullptr; // Commented out as requested
// SDL_Rect remoteSignatureRect = {0, 0, SIGNATURE_WIDTH, SIGNATURE_HEIGHT};
// uint8_t pendingSignatureData[256];
// volatile bool hasPendingSignature = false;

// Remote Clients (Signatures + Cursors)
struct RemoteClient {
    int x, y;
    uint8_t r, g, b; // <--- Add these
    SDL_Texture* sigTexture;
    bool hasSignature;
};
std::map<int, RemoteClient> remoteClients;
pthread_mutex_t remoteClientsMutex = PTHREAD_MUTEX_INITIALIZER;
int myUserId = 0; // Assigned by server via MSG_SIGNATURE broadcast or similar mechanism?
int lastSentX = -1;
int lastSentY = -1;
volatile bool pendingSigUpdate = false;
struct PendingSig {
    int user_id;
    uint8_t data[256];
};
std::vector<PendingSig> pendingSignatures;
pthread_mutex_t sigMutex = PTHREAD_MUTEX_INITIALIZER;


// Layer system - each layer is CANVAS_WIDTH * CANVAS_HEIGHT * 4 bytes (RGBA)
#define MAX_LAYERS 15
#define MAX_UNDO_HISTORY 15

// Canvas state
int currentCanvasId = 0;
int currentLayerId = 1;  // Start on layer 1 (not layer 0 which is paper)
int layerCount = 2;      // Layer 0 (paper) + Layer 1 (drawable)
int layerDisplayIds[MAX_LAYERS]; // Maps index -> original ID
uint8_t layerOpacity[MAX_LAYERS]; // Opacity for each layer (0-255)
int loggedin = 0;
volatile int running = 1;
uint8_t* layers[MAX_LAYERS] = {nullptr};  // Layer 0 = paper (white), Layer 1+ = drawable
// GPU mirrors of the layers
SDL_Texture* layerTextures[MAX_LAYERS] = {nullptr};

// --- DIRTY RECT TRACKING ---
// Keeps track of the bounding box that needs updating for each layer
SDL_Rect layerDirtyRects[MAX_LAYERS]; 
bool layerIsDirty[MAX_LAYERS] = { false };

// Helper to reset rects at startup
void init_dirty_rects() {
    for (int i=0; i<MAX_LAYERS; i++) {
        layerDirtyRects[i] = {CANVAS_WIDTH, CANVAS_HEIGHT, 0, 0}; // Inverted init
        layerIsDirty[i] = false;
    }
}

// Helper to expand the dirty area
void mark_layer_dirty(int layer_id, int x, int y, int brushSize) {
    if (layer_id < 0 || layer_id >= MAX_LAYERS) return;
    
    // Add padding to ensure we catch anti-aliased edges
    int padding = brushSize / 2 + 2; 
    
    int minX = max(0, x - padding);
    int minY = max(0, y - padding);
    int maxX = min(CANVAS_WIDTH, x + padding);
    int maxY = min(CANVAS_HEIGHT, y + padding);

    SDL_Rect* r = &layerDirtyRects[layer_id];
    
    // Expand the rect to include this new stroke
    if (minX < r->x) r->x = minX;
    if (minY < r->y) r->y = minY;
    if (maxX > r->w) r->w = maxX; // Storing Max X in w temp
    if (maxY > r->h) r->h = maxY; // Storing Max Y in h temp
    
    layerIsDirty[layer_id] = true;
}

uint8_t* compositeCanvas = nullptr;       // Final composited image for display
pthread_mutex_t layerMutex = PTHREAD_MUTEX_INITIALIZER;  // Protects layers array and layerCount

// Flag for pending UI updates (set by TCP thread, handled by main thread)
volatile bool pendingLayerUpdate = false;

// Flag to auto-select new layer if we requested it
volatile bool pendingMyNewLayer = false;

// Flags to prevent double-application of layer ops during Undo/Redo
volatile int ignore_layer_add = 0;
volatile int ignore_layer_del = 0;

// NEW UNDO SYSTEM
std::vector<Command*> undoStack;
std::vector<Command*> redoStack;
PaintCommand* currentPaintCmd = nullptr;

// Helper to clean up memory
void clear_redo_stack() {
    for (auto* cmd : redoStack) delete cmd;
    redoStack.clear();
}

void push_undo_command(Command* cmd) {
    undoStack.push_back(cmd);
    if (undoStack.size() > MAX_UNDO_HISTORY) {
        delete undoStack.front();
        undoStack.erase(undoStack.begin());
    }
    lastActionTime = SDL_GetTicks(); // Update timestamp
}

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

/*****************************************************************************
   FORWARD DECLARATIONS
 *****************************************************************************/

void send_tcp_login(const char* username);
void send_tcp_save();
void send_tcp_add_layer(int layer_id);
void send_tcp_delete_layer(int layer_id);
void send_tcp_layer_sync(int layer_id);
void send_all_layers_sync();
void send_udp_draw(int x, int y, int pressure = 255, int angle = 0);
void* tcp_receiver_thread(void* arg);
void* udp_receiver_thread(void* arg);
void init_layer(int layer_idx, bool white);
void perform_undo();
void perform_redo();
void move_layer_local(int layer_id, int dx, int dy);
void send_tcp_layer_move(int layer_id, int dx, int dy);

// TCP thread handle
pthread_t tcp_receiver_tid = 0;

/*****************************************************************************
   NETWORK FUNCTIONS
 *****************************************************************************/

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

// --- SIGNATURE IMPLEMENTATION START ---
// Helper to compress signature to 128 bytes (39x13 2-bit grayscale)
bool compress_signature(uint8_t* out_buffer) {
    if (!signatureTexture) return false;
    
    // 1. Force the rendering to complete before reading
    SDL_SetRenderTarget(renderer, signatureTexture);
    // (Optional: Draw a tiny invisible point to force context update)
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
    SDL_RenderDrawPoint(renderer, 0, 0); 
    
    uint8_t* raw_pixels = new uint8_t[SIGNATURE_WIDTH * SIGNATURE_HEIGHT * 4];
    
    // Use ABGR8888 so that the memory layout is [R, G, B, A] on Little Endian
    if (SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_ABGR8888, raw_pixels, SIGNATURE_WIDTH * 4) != 0) {
        printf("[Client][Signature] Failed to read pixels: %s\n", SDL_GetError());
        delete[] raw_pixels;
        SDL_SetRenderTarget(renderer, NULL);
        return false;
    }
    
    memset(out_buffer, 0, 256);
    int setBits = 0;
    
    // Grid: 45x15 = 675 blocks. 
    // 675 * 2 bits = 1350 bits = 169 bytes.
    // Fits in 256 bytes.
    
    // Block size: 10x10
    for (int y = 0; y < 15; y++) {
        for (int x = 0; x < 45; x++) {
            // Average 15x15 block -> CHANGED to 5x5 block for higher resolution
            // 450 / 5 = 90 blocks wide
            // 150 / 5 = 30 blocks high
            // Total blocks = 2700
            // 2700 * 2 bits = 5400 bits = 675 bytes.
            // Wait, we only have 128 bytes in the payload?
            // The struct TCPMessage has char data[256].
            // We can use up to 256 bytes.
            
            // Let's try 10x10 blocks.
            // 450 / 10 = 45 blocks wide
            // 150 / 10 = 15 blocks high
            // Total = 675 blocks.
            // 675 * 2 bits = 1350 bits = 169 bytes. Fits in 256!
            
            // Previous was 15x15 blocks -> 30x10 grid = 300 blocks = 75 bytes.
            
            // NEW LOGIC: 10x10 blocks
            int sumAlpha = 0;
            for (int dy = 0; dy < 10; dy++) {
                for (int dx = 0; dx < 10; dx++) {
                    int sx = x * 10 + dx;
                    int sy = y * 10 + dy;
                    int idx = (sy * SIGNATURE_WIDTH + sx) * 4;
                    sumAlpha += raw_pixels[idx + 3]; // Alpha
                }
            }
            
            // Average alpha (0-255)
            int avg = sumAlpha / 100;
            
            // Quantize to 2 bits (0, 1, 2, 3)
            uint8_t val = avg / 64;
            if (val > 0) setBits++;
            
            // Pack into buffer
            // Grid is 45 x 15
            int pixelIdx = y * 45 + x;
            int byteIdx = pixelIdx / 4;
            int shift = (3 - (pixelIdx % 4)) * 2; // MSB first
            
            if (byteIdx < 256) {
                out_buffer[byteIdx] |= (val << shift);
            }
        }
    }
    
    printf("[Client][Signature] Compressed signature: %d blocks active (2-bit grayscale)\n", setBits);
    
    delete[] raw_pixels;
    SDL_SetRenderTarget(renderer, NULL);
    return true;
}

void send_tcp_signature() {
    if (!signatureTexture) return;
    
    // Increased buffer size to 256 to match TCPMessage data size
    uint8_t compressed[256];
    if (compress_signature(compressed)) {
        TCPMessage msg;
        memset(&msg, 0, sizeof(msg));
        msg.type = MSG_SIGNATURE;
        msg.canvas_id = currentCanvasId;
        msg.data_len = 256; // Use full capacity
        memcpy(msg.data, compressed, 256);
        
        if (send(tcpSock, &msg, sizeof(msg), 0) < 0) {
            perror("[Client][TCP] Signature send failed");
        } else {
            printf("[Client][TCP] Sent signature (256 bytes)\n");
        }
    }
}
// --- SIGNATURE IMPLEMENTATION END ---

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
    
    // Send signature immediately after login
    send_tcp_signature();
}

// Generalized TCP message sender
bool send_tcp(int type, int layer_id = 0, const char* extra_data = nullptr, size_t extra_len = 0) {
    if (tcpSock < 0) return false;
    
    TCPMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = type;
    msg.canvas_id = currentCanvasId;
    msg.layer_id = layer_id;
    msg.data_len = 0;
    
    if (send(tcpSock, &msg, sizeof(msg), 0) < 0) {
        perror("[Client][TCP] Send failed");
        return false;
    }
    
    // Send extra data if provided
    if (extra_data && extra_len > 0) {
        size_t total_sent = 0;
        while (total_sent < extra_len) {
            ssize_t sent = send(tcpSock, extra_data + total_sent, extra_len - total_sent, 0);
            if (sent < 0) {
                perror("[Client][TCP] Extra data send failed");
                return false;
            }
            total_sent += sent;
        }
    }
    
    return true;
}

void send_tcp_save() {
    printf("[Client][TCP] Sending save request...\n");
    if (send_tcp(MSG_SAVE)) {
        printf("[Client][TCP] Save request sent\n");
    }
}

void send_tcp_add_layer(int layer_id) {
    printf("[Client][TCP] Sending add layer request: layer=%d\n", layer_id);
    
    // FLAG: We asked for this, so we want to switch to it when it arrives
    pendingMyNewLayer = true; 
    
    if (send_tcp(MSG_LAYER_ADD, layer_id)) {
        printf("[Client][TCP] Add layer request sent\n");
    }
}

void send_tcp_delete_layer(int layer_id) {
    printf("[Client][TCP] Sending delete layer request: layer=%d\n", layer_id);
    if (send_tcp(MSG_LAYER_DEL, layer_id)) {
        printf("[Client][TCP] Delete layer request sent\n");
    }
}

void send_tcp_layer_sync(int layer_id) {
    if (layer_id <= 0 || layer_id >= MAX_LAYERS || !layers[layer_id]) return;
    
    printf("[Client][TCP] Sending layer sync: layer=%d\n", layer_id);
    
    size_t layer_size = CANVAS_WIDTH * CANVAS_HEIGHT * 4;
    if (send_tcp(MSG_LAYER_SYNC, layer_id, (const char*)layers[layer_id], layer_size)) {
        printf("[Client][TCP] Layer sync sent (%zu bytes)\n", layer_size);
    }
}

void send_all_layers_sync() {
    // Send all drawable layers to server for sync
    for (int l = 1; l < layerCount && l < MAX_LAYERS; l++) {
        if (layers[l]) {
            send_tcp_layer_sync(l);
        }
    }
}

void send_tcp_reorder_layer(int old_idx, int new_idx) {
    printf("[Client][TCP] Sending reorder layer: %d -> %d\n", old_idx, new_idx);
    
    if (tcpSock < 0) return;
    
    TCPMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_LAYER_REORDER;
    msg.canvas_id = currentCanvasId;
    msg.layer_id = 0;
    msg.data[0] = (char)old_idx;
    msg.data[1] = (char)new_idx;
    msg.data_len = 2;
    
    if (send(tcpSock, &msg, sizeof(msg), 0) < 0) {
        perror("[Client][TCP] Reorder send failed");
    } else {
        printf("[Client][TCP] Reorder request sent\n");
    }
}

void send_tcp_layer_move(int layer_id, int dx, int dy) {
    if (tcpSock < 0) return;
    TCPMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_LAYER_MOVE;
    msg.canvas_id = currentCanvasId;
    msg.layer_id = layer_id;
    
    struct MoveData { int dx; int dy; } payload = { dx, dy };
    memcpy(msg.data, &payload, sizeof(payload));
    msg.data_len = sizeof(payload); 
    
    if (send(tcpSock, &msg, sizeof(msg), 0) < 0) {
        perror("[Client] Failed to send Layer Move");
    }
}

void send_udp_draw(int x, int y, int pressure, int angle) {
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

    lastSentPressure = pressure; // Update global tracker

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
    pkt.pressure = pressure;
    pkt.ex = (int16_t)angle;

    sendto(udpSock, &pkt, sizeof(pkt), 0, (struct sockaddr*)&serverUdpAddr, sizeof(serverUdpAddr));
    
    // apply locally to the correct layer for immediate feedback
    if (currentBrushId < (int)availableBrushes.size()) {
        SDL_Color col = userColor;
        int layer_idx = currentLayerId;
        bool isEraser = (currentBrushId == BRUSH_ERASER_ID);
        bool isSoftEraser = (currentBrushId == BRUSH_SOFT_ERASER_ID);
        
        // Apply pressure locally for prediction if it's the pressure brush
        int effectiveSize = availableBrushes[currentBrushId]->size;
        if (currentBrushId == BRUSH_PRESSURE_ID) {
            effectiveSize = (effectiveSize * pressure) / 255;
            if (effectiveSize < 1) effectiveSize = 1;
        }
        
        availableBrushes[currentBrushId]->paint(x, y, col, effectiveSize, pressure, angle,
            [layer_idx, isEraser, isSoftEraser](int px, int py, SDL_Color c) {
                if (px >= 0 && px < CANVAS_WIDTH && py >= 0 && py < CANVAS_HEIGHT) {
                    int idx = (py * CANVAS_WIDTH + px) * 4;
                    
                    if (isEraser) {
                        // Eraser: Overwrite with transparent
                        layers[layer_idx][idx]     = 0;
                        layers[layer_idx][idx + 1] = 0;
                        layers[layer_idx][idx + 2] = 0;
                        layers[layer_idx][idx + 3] = 0;
                        return;
                    }

                    if (isSoftEraser) {
                        uint8_t currentAlpha = layers[layer_idx][idx + 3];
                        uint8_t eraseStrength = c.a; // "Strength" comes from brush alpha

                        // Subtract strength from current alpha
                        if (currentAlpha > 0) {
                            int newAlpha = (int)currentAlpha - (int)eraseStrength;
                            if (newAlpha < 0) newAlpha = 0;
                            
                            layers[layer_idx][idx + 3] = (uint8_t)newAlpha;
                            
                            // Optional: If alpha is 0, clear RGB to keep memory clean
                            if (newAlpha == 0) {
                                layers[layer_idx][idx] = 0;
                                layers[layer_idx][idx+1] = 0;
                                layers[layer_idx][idx+2] = 0;
                            }
                        }
                        return;
                    }
                    
                    // Alpha Blending (Source Over Destination)
                    uint8_t dst_r = layers[layer_idx][idx];
                    uint8_t dst_g = layers[layer_idx][idx + 1];
                    uint8_t dst_b = layers[layer_idx][idx + 2];
                    uint8_t dst_a = layers[layer_idx][idx + 3];
                    
                    uint8_t src_r = c.r;
                    uint8_t src_g = c.g;
                    uint8_t src_b = c.b;
                    uint8_t src_a = c.a;
                    
                    if (src_a == 255) {
                        layers[layer_idx][idx]     = src_r;
                        layers[layer_idx][idx + 1] = src_g;
                        layers[layer_idx][idx + 2] = src_b;
                        layers[layer_idx][idx + 3] = src_a;
                    } else if (src_a > 0) {
                        float sa = src_a / 255.0f;
                        float da = dst_a / 255.0f;
                        float out_a = sa + da * (1.0f - sa);
                        
                        if (out_a > 0.0f) {
                            float out_r = (src_r * sa + dst_r * da * (1.0f - sa)) / out_a;
                            float out_g = (src_g * sa + dst_g * da * (1.0f - sa)) / out_a;
                            float out_b = (src_b * sa + dst_b * da * (1.0f - sa)) / out_a;
                            
                            layers[layer_idx][idx]     = (uint8_t)out_r;
                            layers[layer_idx][idx + 1] = (uint8_t)out_g;
                            layers[layer_idx][idx + 2] = (uint8_t)out_b;
                            layers[layer_idx][idx + 3] = (uint8_t)(out_a * 255.0f);
                        }
                    }
                }
            });
        mark_layer_dirty(layer_idx, x, y, effectiveSize);
    }
}

void send_udp_cursor(int x, int y) {
    if (udpSock < 0) return;

    UDPMessage pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = MSG_CURSOR;
    pkt.x = x;
    pkt.y = y;
    pkt.brush_id = myUserId; // Send our ID in the brush_id field
    pkt.r = userColor.r;
    pkt.g = userColor.g;
    pkt.b = userColor.b;
    pkt.a = 255;
    pkt.pressure = 255;  // Full pressure doesnt really matter it's just cursor

    sendto(udpSock, &pkt, sizeof(pkt), 0, (struct sockaddr*)&serverUdpAddr, sizeof(serverUdpAddr));
}

/*****************************************************************************
   THREAD FUNCTIONS
 *****************************************************************************/

void* tcp_receiver_thread(void* arg) {
    (void)arg;
    printf("[Client][TCP-Thread] Started receiver thread\n");
    
    TCPMessage msg;
    while (running) {
        memset(&msg, 0, sizeof(msg));
        ssize_t n = recv(tcpSock, &msg, sizeof(msg), 0);
        if (n <= 0) {
            if (running && loggedin) {
                printf("[Client][TCP-Thread] Connection closed or error. Shutting down.\n");
                running = 0; // Signal main loop to exit
                
                // Push a quit event to wake up the main loop if it's waiting
                SDL_Event event;
                event.type = SDL_QUIT;
                event.quit.timestamp = SDL_GetTicks();
                SDL_PushEvent(&event);
            } else {
                printf("[Client][TCP-Thread] Socket closed (Logout).\n");
            }
            break;
        }

        printf("[Client][TCP-Thread] Received message: type=%d, canvas=%d, data_len=%d\n",
               msg.type, msg.canvas_id, msg.data_len);

        switch (msg.type) {
            case MSG_WELCOME:
                printf("[Client][TCP-Thread] WELCOME received! Canvas #%d, layers=%d, UID=%d\n", 
                       msg.canvas_id, msg.layer_count, msg.user_id);
                
                loggedin = 1;
                myUserId = msg.user_id;
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
                            layerIsDirty[l] = true; // Force GPU upload
                            layerDirtyRects[l] = {0, 0, CANVAS_WIDTH, CANVAS_HEIGHT};
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
                
                // --- RESIZE WINDOW TO CANVAS MODE ---
                SDL_SetWindowSize(window, CANVAS_WIDTH, CANVAS_HEIGHT);
                SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
                windowWidth = CANVAS_WIDTH;
                windowHeight = CANVAS_HEIGHT;
                SetupUI();
                
                UpdateLayerButtons();
                break;

            // --- SIGNATURE IMPLEMENTATION START ---
            case MSG_SIGNATURE:
                printf("[Client][TCP-Thread] Received signature for UID=%d\n", msg.user_id);
                pthread_mutex_lock(&sigMutex);
                {
                    PendingSig ps;
                    ps.user_id = msg.user_id;
                    memcpy(ps.data, msg.data, 256); // Updated to 256 bytes
                    pendingSignatures.push_back(ps);
                    pendingSigUpdate = true;
                    
                    // If this is the first time we see a signature and we don't have an ID yet,
                    // and we just sent ours, maybe this is ours?
                    // The server echoes our signature back.
                    // We can assume the first one we get after login that matches our data is ours,
                    // OR we just treat all of them as remote, and if we happen to draw our own, so be it.
                    // But the requirement says "client cannot see their own signature".
                    // We'll handle this in the main thread by checking if we are the sender.
                    // Wait, we don't know if we are the sender unless we know our ID.
                    // We'll assume the server sends us our ID in the MSG_SIGNATURE packet.
                    // If we haven't set myUserId yet, and we just sent a signature, this might be it.
                    // But simpler: The server sends existing clients first. Then ours.
                    // Actually, we can just set myUserId when we receive the echo of our own signature.
                    // But how do we know it's ours?
                    // We'll assume the last one received after we send is ours? No.
                    // We'll just render all other IDs. We need to know our ID.
                    // Let's assume for now we render everyone else.
                    // If we see a cursor moving that matches our mouse, that's us.
                    // But we need to filter by ID.
                    // Let's assume the server sends a special "You are ID X" packet or we infer it.
                    // For now, we'll store all of them.
                    if (myUserId == 0) {
                         // Heuristic: If we just logged in, and we receive a signature that matches what we sent...
                         // But we don't have what we sent easily accessible here.
                         // Let's just store it.
                         // Actually, we can use the fact that we don't receive cursor updates for ourselves via UDP usually?
                         // No, UDP is broadcast to everyone usually.
                         // If we receive a cursor update with ID X, and it matches our local mouse position exactly...
                         // We'll handle ID assignment in main thread logic if possible.
                         // For now, just store.
                         if (myUserId == 0) myUserId = msg.user_id; // First one is us? No, existing users come first.
                         // Actually, we can't easily know our ID without a dedicated packet.
                         // But we can just render all signatures that have a corresponding cursor update.
                         // We won't receive cursor updates for ourselves from the server if the server filters them?
                         // Server code: "if (sock != client_sock)" for TCP, but for UDP "broadcast_udp" sends to all?
                         // Let's check server broadcast_udp.
                    }
                }
                pthread_mutex_unlock(&sigMutex);
                break;
            // --- SIGNATURE IMPLEMENTATION END ---

            case MSG_CANVAS_DATA:
                printf("[Client][TCP-Thread] CANVAS_DATA received: %d bytes\n", msg.data_len);
                break;

            case MSG_LAYER_ADD:
                printf("[Client][TCP-Thread] LAYER_ADD confirmed: new layer count=%d\n", msg.layer_count);
                pthread_mutex_lock(&layerMutex);
                
                if (ignore_layer_add > 0) {
                    printf("[Client][TCP-Thread] Ignoring LAYER_ADD (self-triggered via Undo/Redo)\n");
                    ignore_layer_add--;
                    layerCount = msg.layer_count;
                } else {
                    layerCount = msg.layer_count;
                    // Create the new layer locally
                    if (layerCount > 1 && layerCount <= MAX_LAYERS && !layers[layerCount - 1]) {
                        init_layer(layerCount - 1, false);
                        printf("[Client][TCP-Thread] Created layer %d locally\n", layerCount - 1);
                    }
                    
                    // --- NEW LOGIC START ---
                    if (pendingMyNewLayer) {
                        // If I requested this, auto-select the new layer
                        // usually the new layer is at the end (layerCount - 1) 
                        // or at msg.layer_id if you support insertion
                        
                        // If msg.layer_id is reliable (index where it was added):
                        currentLayerId = msg.layer_id; 
                        
                        // Or if you always append to top:
                        // currentLayerId = layerCount - 1;

                        printf("[Client][TCP-Thread] Auto-selecting my new layer: %d\n", currentLayerId);
                        pendingMyNewLayer = false; // Reset flag
                    }
                    // --- NEW LOGIC END ---
                }
                pendingLayerUpdate = true;  // Main thread will call UpdateLayerButtons()
                pthread_mutex_unlock(&layerMutex);
                break;

            case MSG_LAYER_DEL:
                printf("[Client][TCP-Thread] LAYER_DEL confirmed: deleted layer %d, new count=%d\n", 
                       msg.layer_id, msg.layer_count);
                pthread_mutex_lock(&layerMutex);
                
                if (ignore_layer_del > 0) {
                    printf("[Client][TCP-Thread] Ignoring LAYER_DEL (self-triggered via Undo/Redo)\n");
                    ignore_layer_del--;
                    layerCount = msg.layer_count;
                } else {
                    // Delete local layer and shift remaining layers down
                    if (msg.layer_id > 0 && msg.layer_id < MAX_LAYERS) {
                        // Delete the layer
                        if (layers[msg.layer_id]) {
                            delete[] layers[msg.layer_id];
                            layers[msg.layer_id] = nullptr;
                        }
                        // Texture destruction is handled in main thread via update_canvas_texture

                        // Shift all layers above down by one
                        for (int l = msg.layer_id; l < MAX_LAYERS - 1; l++) {
                            layers[l] = layers[l + 1];
                            layerOpacity[l] = layerOpacity[l + 1];
                            layerTextures[l] = layerTextures[l + 1];
                            layerIsDirty[l] = true; // Force re-upload to be safe
                            layerDirtyRects[l] = {0, 0, CANVAS_WIDTH, CANVAS_HEIGHT};
                        }
                        layers[MAX_LAYERS - 1] = nullptr;
                        layerOpacity[MAX_LAYERS - 1] = 255; // Reset opacity for new empty slot
                        layerTextures[MAX_LAYERS - 1] = nullptr;
                        layerIsDirty[MAX_LAYERS - 1] = false;
                        printf("[Client][TCP-Thread] Shifted layers down after deleting layer %d\n", msg.layer_id);
                    }
                    layerCount = msg.layer_count;
                }
                
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
                            layerIsDirty[layer_idx] = true;
                            layerDirtyRects[layer_idx] = {0, 0, CANVAS_WIDTH, CANVAS_HEIGHT};
                        } else {
                            printf("[Client][TCP-Thread] Layer sync incomplete: %zu/%zu bytes\n", received, layer_size);
                        }
                        
                        pthread_mutex_unlock(&layerMutex);
                    }
                }
                break;

            case MSG_LAYER_REORDER:
                {
                    int old_idx = (uint8_t)msg.data[0];
                    int new_idx = (uint8_t)msg.data[1];
                    printf("[Client][TCP-Thread] LAYER_REORDER: %d -> %d\n", old_idx, new_idx);
                    
                    pthread_mutex_lock(&layerMutex);
                    if (old_idx > 0 && old_idx < MAX_LAYERS && new_idx > 0 && new_idx < MAX_LAYERS && layers[old_idx]) {
                        // Swap logic: shift layers between old and new
                        uint8_t* movingLayer = layers[old_idx];
                        int movingId = layerDisplayIds[old_idx];
                        int movingOpacity = layerOpacity[old_idx];
                        SDL_Texture* movingTexture = layerTextures[old_idx];
                        bool movingDirty = layerIsDirty[old_idx];
                        
                        if (old_idx < new_idx) {
                            for (int i = old_idx; i < new_idx; i++) {
                                layers[i] = layers[i+1];
                                layerDisplayIds[i] = layerDisplayIds[i+1];
                                layerOpacity[i] = layerOpacity[i+1];
                                layerTextures[i] = layerTextures[i+1];
                                layerIsDirty[i] = layerIsDirty[i+1];
                            }
                        } else {
                            for (int i = old_idx; i > new_idx; i--) {
                                layers[i] = layers[i-1];
                                layerDisplayIds[i] = layerDisplayIds[i-1];
                                layerOpacity[i] = layerOpacity[i-1];
                                layerTextures[i] = layerTextures[i-1];
                                layerIsDirty[i] = layerIsDirty[i-1];
                            }
                        }
                        layers[new_idx] = movingLayer;
                        layerDisplayIds[new_idx] = movingId;
                        layerOpacity[new_idx] = movingOpacity;
                        layerTextures[new_idx] = movingTexture;
                        layerIsDirty[new_idx] = movingDirty;
                        
                        // Update current selection if needed
                        if (currentLayerId == old_idx) currentLayerId = new_idx;
                        else if (old_idx < new_idx && currentLayerId > old_idx && currentLayerId <= new_idx) currentLayerId--;
                        else if (old_idx > new_idx && currentLayerId >= new_idx && currentLayerId < old_idx) currentLayerId++;
                        
                        pendingLayerUpdate = true;
                    }
                    pthread_mutex_unlock(&layerMutex);
                }
                break;

            case MSG_LAYER_MOVE:
                {
                    struct MoveData { int dx; int dy; } payload;
                    memcpy(&payload, msg.data, sizeof(MoveData));
                    
                    printf("[Client][TCP-Thread] LAYER_MOVE: layer=%d dx=%d dy=%d\n", msg.layer_id, payload.dx, payload.dy);

                    pthread_mutex_lock(&layerMutex);
                    // Apply the move
                    move_layer_local(msg.layer_id, payload.dx, payload.dy);
                    pthread_mutex_unlock(&layerMutex);
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
                            bool isEraser = (pkt->brush_id == BRUSH_ERASER_ID);
                            bool isSoftEraser = (pkt->brush_id == BRUSH_SOFT_ERASER_ID);
                            int angle = pkt->ex;
                            
                            availableBrushes[pkt->brush_id]->paint(pkt->x, pkt->y, col, brushSize, pkt->pressure, angle,
                                [layer_idx, isEraser, isSoftEraser](int px, int py, SDL_Color c) {
                                    if (px >= 0 && px < CANVAS_WIDTH && py >= 0 && py < CANVAS_HEIGHT) {
                                        int idx = (py * CANVAS_WIDTH + px) * 4;
                                        
                                        if (isEraser) {
                                            // Eraser: Overwrite with transparent
                                            layers[layer_idx][idx]     = 0;
                                            layers[layer_idx][idx + 1] = 0;
                                            layers[layer_idx][idx + 2] = 0;
                                            layers[layer_idx][idx + 3] = 0;
                                            return;
                                        }

                                        if (isSoftEraser) {
                                            uint8_t currentAlpha = layers[layer_idx][idx + 3];
                                            uint8_t eraseStrength = c.a;
                                            if (currentAlpha > 0) {
                                                int newAlpha = (int)currentAlpha - (int)eraseStrength;
                                                if (newAlpha < 0) newAlpha = 0;
                                                layers[layer_idx][idx + 3] = (uint8_t)newAlpha;
                                            }
                                            return;
                                        }
                                        
                                        // Simple alpha blending with existing pixel
                                        uint8_t oldR = layers[layer_idx][idx];
                                        uint8_t oldG = layers[layer_idx][idx + 1];
                                        uint8_t oldB = layers[layer_idx][idx + 2];
                                        uint8_t oldA = layers[layer_idx][idx + 3];
                                        
                                        // If new pixel is fully opaque, just overwrite
                                        if (c.a == 255) {
                                            layers[layer_idx][idx]     = c.r;
                                            layers[layer_idx][idx + 1] = c.g;
                                            layers[layer_idx][idx + 2] = c.b;
                                            layers[layer_idx][idx + 3] = c.a;
                                        } else {
                                            // Standard alpha blending: src over dst
                                            // outA = srcA + dstA * (1 - srcA)
                                            // outRGB = (srcRGB * srcA + dstRGB * dstA * (1 - srcA)) / outA
                                            
                                            float srcA = c.a / 255.0f;
                                            float dstA = oldA / 255.0f;
                                            float outA = srcA + dstA * (1.0f - srcA);
                                            
                                            if (outA > 0.0f) {
                                                float outR = (c.r * srcA + oldR * dstA * (1.0f - srcA)) / outA;
                                                float outG = (c.g * srcA + oldG * dstA * (1.0f - srcA)) / outA;
                                                float outB = (c.b * srcA + oldB * dstA * (1.0f - srcA)) / outA;
                                                
                                                layers[layer_idx][idx]     = (uint8_t)outR;
                                                layers[layer_idx][idx + 1] = (uint8_t)outG;
                                                layers[layer_idx][idx + 2] = (uint8_t)outB;
                                                layers[layer_idx][idx + 3] = (uint8_t)(outA * 255.0f);
                                            }
                                        }
                                    }
                                });
                            
                            pthread_mutex_lock(&layerMutex);
                            mark_layer_dirty(layer_idx, pkt->x, pkt->y, brushSize);
                            pthread_mutex_unlock(&layerMutex);
                        }
                    }
                    break;

                case MSG_CURSOR:
                    // Remote cursor
                    {
                        // Update remote cursor position
                        // Use brush_id as user_id
                        int uid = pkt->brush_id;
                        // printf("UDP Cursor Update: UID=%d  X=%d Y=%d\n", uid, pkt->x, pkt->y);
                        if (uid != myUserId) { // Don't track ourselves
                            // printf("[Client][UDP] Updating cursor for UID %d (My ID: %d)\n", uid, myUserId);
                            pthread_mutex_lock(&remoteClientsMutex);
                            remoteClients[uid].x = pkt->x;
                            remoteClients[uid].y = pkt->y;
                            
                            // <--- Add these lines to update color
                            remoteClients[uid].r = pkt->r;
                            remoteClients[uid].g = pkt->g;
                            remoteClients[uid].b = pkt->b;
                            
                            // remoteClients[uid].hasSignature = (remoteClients[uid].sigTexture != nullptr); // REMOVED: Race condition
                            pthread_mutex_unlock(&remoteClientsMutex);
                        }
                        
                        // Also update legacy remote_cursors map for compatibility if needed
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

/*****************************************************************************
   DRAWING FUNCTIONS
 *****************************************************************************/

void init_layer(int layer_idx, bool white) {
    if (layer_idx < 0 || layer_idx >= MAX_LAYERS) return;
    
    if (layers[layer_idx]) {
        delete[] layers[layer_idx];
    }
    layers[layer_idx] = new uint8_t[CANVAS_WIDTH * CANVAS_HEIGHT * 4];
    
    if (white) {
        // White opaque (paper)
        memset(layers[layer_idx], 255, CANVAS_WIDTH * CANVAS_HEIGHT * 4);
    } else {
        // Fully transparent
        memset(layers[layer_idx], 0, CANVAS_WIDTH * CANVAS_HEIGHT * 4);
    }

    // Mark for GPU Init (Do NOT call SDL here)
    layerIsDirty[layer_idx] = true;
    layerDirtyRects[layer_idx] = {0, 0, CANVAS_WIDTH, CANVAS_HEIGHT};
}

// Wrappers for UI buttons
void record_add_layer_command() {
    // Adding is tricky because we don't know the ID yet.
    // For now, we'll just clear redo stack to avoid inconsistencies.
    clear_redo_stack();
}

void record_delete_layer_command(int layer_id) {
    clear_redo_stack();
    DeleteLayerCommand* cmd = new DeleteLayerCommand(layer_id, CANVAS_WIDTH, CANVAS_HEIGHT);
    push_undo_command(cmd);
}

void perform_undo() {
    if (undoStack.empty()) {
        printf("[Client] Nothing to undo.\n");
        return;
    }

    // Check Timeout
    uint32_t now = SDL_GetTicks();
    if (now - lastActionTime > UNDO_TIMEOUT_MS) {
        printf("[Client] Undo expired! (Time limit exceeded)\n");
        // Optional: Clear stack to prevent confusion?
        // clear_undo_stack(); 
        return;
    }

    // 1. Get last command
    Command* cmd = undoStack.back();
    undoStack.pop_back();

    // 2. Execute Undo Logic
    cmd->undo(); 
    // This calls local memcpy AND sends result to server via TCP
    
    // 3. Move to Redo Stack
    redoStack.push_back(cmd);
    
    // Mark layer as dirty
    for(int i=0; i<MAX_LAYERS; i++) {
        layerIsDirty[i] = true;
        layerDirtyRects[i] = {0, 0, CANVAS_WIDTH, CANVAS_HEIGHT};
    }
    
    printf("[Client] Undid action.\n");
    lastActionTime = now; // Reset timer on action
}

void perform_redo() {
    if (redoStack.empty()) {
        printf("[Client] Nothing to redo.\n");
        return;
    }

    // 1. Get command
    Command* cmd = redoStack.back();
    redoStack.pop_back();

    // 2. Execute Redo Logic
    cmd->redo();
    
    // 3. Move back to Undo Stack
    push_undo_command(cmd);

    // Mark layer as dirty
    for(int i=0; i<MAX_LAYERS; i++) {
        layerIsDirty[i] = true;
        layerDirtyRects[i] = {0, 0, CANVAS_WIDTH, CANVAS_HEIGHT};
    }

    printf("[Client] Redid action.\n");
}

// --- MENU UI GLOBALS ---
struct MenuLayer {
    uint8_t* pixels;
    MenuLayer() : pixels(nullptr) {}
};
std::vector<MenuLayer> menuLayers;
SDL_Texture* menuTexture = nullptr;
int menuWidth = MENU_WIDTH;
int menuHeight = MENU_HEIGHT;

// Animation State
uint32_t lastMenuAnimTime = 0;
int currentMenuFrame = 0; // 0 = Frame A (Layer 13), 1 = Frame B (Layer 14)

// --- HELPER FUNCTIONS FOR LOADING UI.JSON ---
static const string b64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

bool is_base64(unsigned char c) {
    return (isalnum(c) || (c == '+') || (c == '/'));
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

void load_menu_ui() {
    printf("[Client][UI] Loading ui.json...\n");
    FILE* f = fopen("ui.json", "r");
    if (!f) { printf("[Client][UI] ui.json not found!\n"); return; }
    
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    char* buffer = (char*)malloc(fsize + 1);
    fread(buffer, 1, fsize, f);
    buffer[fsize] = 0;
    fclose(f);
    
    string json = buffer;
    free(buffer);

    // Simple parser loop to find "data" fields
    size_t pos = 0;
    while ((pos = json.find("\"data\":", pos)) != string::npos) {
        size_t start = json.find("\"", pos + 7) + 1;
        size_t end = json.find("\"", start);
        string b64 = json.substr(start, end - start);
        
        // Decode
        vector<unsigned char> compressed = base64_decode(b64);
        vector<uint8_t> data = packbits_decompress(compressed);
        
        // FIX: Check against MENU dimensions, not CANVAS dimensions
        if (data.size() != MENU_WIDTH * MENU_HEIGHT * 4) {
            pos = end;
            continue;
        }

        // Store Layer
        MenuLayer layer;
        layer.pixels = new uint8_t[MENU_WIDTH * MENU_HEIGHT * 4];
        
        // Convert uint32_t (ABGR/RGBA) to uint8_t array
        uint32_t* src = (uint32_t*)data.data();
        for (int i = 0; i < MENU_WIDTH * MENU_HEIGHT; i++) {
            uint32_t p = src[i];
            // Assuming server format: R G B A (Big Endian packed) or similar
            // Adjust shifting based on your server's encode_layer.
            // Server usually packs: (r<<24)|(g<<16)|(b<<8)|a
            int idx = i * 4;
            layer.pixels[idx]   = (p >> 24) & 0xFF; // R
            layer.pixels[idx+1] = (p >> 16) & 0xFF; // G
            layer.pixels[idx+2] = (p >> 8)  & 0xFF; // B
            layer.pixels[idx+3] = (p)       & 0xFF; // A
        }
        
        menuLayers.push_back(layer);
        pos = end;
    }
    printf("[Client][UI] Loaded %zu layers for menu background\n", menuLayers.size());
}

void update_menu_texture() {
    if (menuLayers.empty()) return;

    // Allocate buffer
    uint8_t* composite = new uint8_t[MENU_WIDTH * MENU_HEIGHT * 4];
    
    // 1. Fill with White (Background)
    memset(composite, 255, MENU_WIDTH * MENU_HEIGHT * 4);

    // 2. Define Layers to Draw
    // Static Base: Layers 1 to 12 (Indices 0 to 11 in vector)
    int staticEnd = min((int)menuLayers.size(), 12);
    
    // Animation Frame: Layer 13 (Index 12) or 14 (Index 13)
    int animLayerIdx = -1;
    if (currentMenuFrame == 0) animLayerIdx = 12; // Layer 13
    else animLayerIdx = 13;                       // Layer 14

    // Helper to blend
    auto blend = [&](int layerIdx) {
        if (layerIdx >= menuLayers.size()) return;
        uint8_t* src = menuLayers[layerIdx].pixels;
        for (int i = 0; i < MENU_WIDTH * MENU_HEIGHT * 4; i+=4) {
            uint8_t srcA = src[i+3];
            if (srcA == 0) continue;
            
            // Simple alpha blend over composite
            float a = srcA / 255.0f;
            composite[i]   = (uint8_t)(src[i] * a   + composite[i] * (1-a));
            composite[i+1] = (uint8_t)(src[i+1] * a + composite[i+1] * (1-a));
            composite[i+2] = (uint8_t)(src[i+2] * a + composite[i+2] * (1-a));
            // Alpha stays 255 (opaque bg)
        }
    };

    // Blend Static Layers
    for (int i = 0; i < staticEnd; i++) blend(i);
    
    // Blend Active Animation Frame
    if (animLayerIdx >= 0 && animLayerIdx < menuLayers.size()) blend(animLayerIdx);

    // Upload to GPU
    if (!menuTexture) {
        menuTexture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, 
                                      SDL_TEXTUREACCESS_STREAMING, 
                                      MENU_WIDTH, MENU_HEIGHT);
    }
    SDL_UpdateTexture(menuTexture, NULL, composite, MENU_WIDTH * 4);
    
    delete[] composite;
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
    
    // Initialize display IDs and Opacity
    for (int i = 0; i < MAX_LAYERS; i++) {
        layerDisplayIds[i] = i;
        layerOpacity[i] = 255;
    }
    
    printf("[Client][Canvas] Canvas initialized with %d layers\n", layerCount);
}

void clear_signature(SDL_Renderer* renderer) {
    if (!signatureTexture) return;
    SDL_SetRenderTarget(renderer, signatureTexture);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0); // Transparent
    SDL_RenderClear(renderer);
    SDL_SetRenderTarget(renderer, NULL);
}

void composite_layers() {
    // Deprecated: GPU compositing is now used.
    // This function is kept empty to satisfy any legacy calls, 
    // but the real work happens in the render loop.
}

void update_canvas_texture() {
    for (int i = 0; i < MAX_LAYERS; i++) {
        // If we have data (layers[i]) but no texture, CREATE it now (Main Thread)
        if (layers[i] && !layerTextures[i]) {
            layerTextures[i] = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, 
                                                 SDL_TEXTUREACCESS_STREAMING, 
                                                 CANVAS_WIDTH, CANVAS_HEIGHT);
            if (layerTextures[i]) {
                SDL_SetTextureBlendMode(layerTextures[i], SDL_BLENDMODE_BLEND);
                layerIsDirty[i] = true; // Force immediate upload
                layerDirtyRects[i] = {0, 0, CANVAS_WIDTH, CANVAS_HEIGHT};
                printf("[Client][Main] Created GPU Texture for Layer %d\n", i);
            }
        }
        
        // If we have NO data but HAVE a texture (Deleted layer), DESTROY it
        if (!layers[i] && layerTextures[i]) {
            SDL_DestroyTexture(layerTextures[i]);
            layerTextures[i] = nullptr;
            printf("[Client][Main] Destroyed GPU Texture for Layer %d\n", i);
        }
    }
}

void download_as_bmp() {
    // Save the flattened canvas as a BMP file locally
    printf("[Client][Download] Saving canvas as BMP...\n");
    
    if (!compositeCanvas) {
        // Allocate if missing
        compositeCanvas = new uint8_t[CANVAS_WIDTH * CANVAS_HEIGHT * 4];
    }
    
    // Perform CPU composition for saving
    // 1. Clear to white (Layer 0)
    for (int i = 0; i < CANVAS_WIDTH * CANVAS_HEIGHT * 4; i += 4) {
        compositeCanvas[i]     = 255;
        compositeCanvas[i + 1] = 255;
        compositeCanvas[i + 2] = 255;
        compositeCanvas[i + 3] = 255;
    }
    
    // 2. Blend all other layers
    for (int l = 1; l < layerCount; l++) {
        if (layers[l]) {
            for (int i = 0; i < CANVAS_WIDTH * CANVAS_HEIGHT * 4; i += 4) {
                uint8_t srcA = layers[l][i + 3];
                if (srcA == 0) continue;
                
                uint8_t srcR = layers[l][i];
                uint8_t srcG = layers[l][i + 1];
                uint8_t srcB = layers[l][i + 2];
                
                if (srcA == 255) {
                    compositeCanvas[i]     = srcR;
                    compositeCanvas[i + 1] = srcG;
                    compositeCanvas[i + 2] = srcB;
                    compositeCanvas[i + 3] = 255;
                } else {
                    // Simple alpha blend over opaque background
                    float a = srcA / 255.0f;
                    compositeCanvas[i]     = (uint8_t)(srcR * a + compositeCanvas[i] * (1.0f - a));
                    compositeCanvas[i + 1] = (uint8_t)(srcG * a + compositeCanvas[i + 1] * (1.0f - a));
                    compositeCanvas[i + 2] = (uint8_t)(srcB * a + compositeCanvas[i + 2] * (1.0f - a));
                }
            }
        }
    }
    
    // Generate filename with timestamp
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    char filename[128];
    snprintf(filename, sizeof(filename), "canvas_%04d%02d%02d_%02d%02d%02d.bmp",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec);
    
    // Create SDL surface from composite canvas (RGBA format)
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormatFrom(
        compositeCanvas,
        CANVAS_WIDTH, CANVAS_HEIGHT,
        32,                           // bits per pixel
        CANVAS_WIDTH * 4,             // pitch (bytes per row)
        SDL_PIXELFORMAT_RGBA32
    );
    
    if (!surface) {
        printf("[Client][Download] ERROR: Failed to create surface: %s\n", SDL_GetError());
        return;
    }
    
    // Save as BMP
    if (SDL_SaveBMP(surface, filename) == 0) {
        printf("[Client][Download] Saved canvas to: %s\n", filename);
    } else {
        printf("[Client][Download] ERROR: Failed to save BMP: %s\n", SDL_GetError());
    }
    
    SDL_FreeSurface(surface);
}

/*****************************************************************************
   BRUSH INITIALIZATION
 *****************************************************************************/

void init_brushes() {
    printf("[Client][Brushes] Initializing brush system...\n");
    
    // Order must match constants:
    // 0: Round
    // 1: Square
    // 2: Hard Eraser
    // 3: Soft Eraser
    // 4: Pressure
    // 5: Airbrush
    // 6: Texture
    
    availableBrushes.push_back(new RoundBrush());       // 0
    availableBrushes.push_back(new SquareBrush());      // 1
    availableBrushes.push_back(new HardEraserBrush());  // 2
    availableBrushes.push_back(new SoftEraserBrush());  // 3
    availableBrushes.push_back(new PressureBrush());    // 4
    availableBrushes.push_back(new Airbrush());         // 5
    availableBrushes.push_back(new TexturedBrush());    // 6
    
    for (int i = 0; i < (int)availableBrushes.size(); i++) {
        availableBrushes[i]->size = 15;
    }
    
    printf("[Client][Brushes] %zu brushes loaded\n", availableBrushes.size());
}

SDL_Color get_composite_pixel(int x, int y) {
    SDL_Color c = {255, 255, 255, 255};
    if (x < 0 || x >= CANVAS_WIDTH || y < 0 || y >= CANVAS_HEIGHT) return c;
    
    int idx = (y * CANVAS_WIDTH + x) * 4;
    float r = 255.0f, g = 255.0f, b = 255.0f; // Start with white background
    
    // Iterate all layers (0 is paper, usually opaque white)
    for (int i = 0; i < layerCount; i++) {
        if (layers[i]) {
            uint8_t srcR = layers[i][idx];
            uint8_t srcG = layers[i][idx+1];
            uint8_t srcB = layers[i][idx+2];
            uint8_t srcA = layers[i][idx+3];
            
            if (srcA == 0) continue;
            
            if (srcA == 255) {
                r = srcR; g = srcG; b = srcB;
            } else {
                float sa = srcA / 255.0f;
                r = srcR * sa + r * (1.0f - sa);
                g = srcG * sa + g * (1.0f - sa);
                b = srcB * sa + b * (1.0f - sa);
            }
        }
    }
    
    c.r = (uint8_t)r;
    c.g = (uint8_t)g;
    c.b = (uint8_t)b;
    c.a = 255;
    return c;
}

/*****************************************************************************
   SDL EVENT HANDLING
 *****************************************************************************/

bool isMovingLayer = false;
int totalMoveX = 0;
int totalMoveY = 0;

void move_layer_local(int layer_id, int dx, int dy) {
    if (layer_id <= 0 || layer_id >= MAX_LAYERS || !layers[layer_id]) return;
    if (dx == 0 && dy == 0) return;

    // Use a temp buffer to avoid overwriting data we need to read
    uint8_t* temp = new uint8_t[CANVAS_WIDTH * CANVAS_HEIGHT * 4];
    memset(temp, 0, CANVAS_WIDTH * CANVAS_HEIGHT * 4); // Clear to transparent

    uint8_t* src = layers[layer_id];

    for (int y = 0; y < CANVAS_HEIGHT; y++) {
        for (int x = 0; x < CANVAS_WIDTH; x++) {
            // Calculate where this pixel comes FROM
            int srcX = x - dx;
            int srcY = y - dy;

            if (srcX >= 0 && srcX < CANVAS_WIDTH && srcY >= 0 && srcY < CANVAS_HEIGHT) {
                int dstIdx = (y * CANVAS_WIDTH + x) * 4;
                int srcIdx = (srcY * CANVAS_WIDTH + srcX) * 4;
                
                temp[dstIdx]     = src[srcIdx];
                temp[dstIdx + 1] = src[srcIdx + 1];
                temp[dstIdx + 2] = src[srcIdx + 2];
                temp[dstIdx + 3] = src[srcIdx + 3];
            }
        }
    }

    // Copy back
    memcpy(layers[layer_id], temp, CANVAS_WIDTH * CANVAS_HEIGHT * 4);
    delete[] temp;
    layerIsDirty[layer_id] = true;
    layerDirtyRects[layer_id] = {0, 0, CANVAS_WIDTH, CANVAS_HEIGHT};
}

void handle_events() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_QUIT:
                printf("[Client][Event] QUIT received\n");
                running = 0;
                break;

            case SDL_MOUSEBUTTONDOWN:
                // Ignore mouse events simulated from touch to avoid double-drawing
                if (e.button.which == SDL_TOUCH_MOUSEID) {
                    // printf("[Client][Input] Ignoring simulated mouse down\n");
                    break;
                }

                if (e.button.button == SDL_BUTTON_LEFT) {
                    // Debug Mouse Event
                    printf("[Client][Input] Mouse Down (Real Mouse) ID=%d\n", e.button.which);

                    int mx = e.button.x;
                    int my = e.button.y;
                    
                    if (!loggedin) {
                        if (handle_login_screen_click(mx, my)) {
                            if (isDrawingSignature) {
                                mouseDown = 1;
                                lastSigX = mx - signatureRect.x;
                                lastSigY = my - signatureRect.y;
                            }
                        }
                    } else {
                        // Only check UI clicks if UI is visible.
                        // If hidden, treat clicks as canvas clicks immediately.
                        bool uiClicked = false;
                        if (uiVisible) {
                            uiClicked = handle_canvas_ui_click(mx, my);
                        }

                        if (!uiClicked) {
                            // CHECK SPACE KEY FOR PANNING
                            if (spacePressed) {
                                isPanning = true;
                                lastMouseX = mx;
                                lastMouseY = my;
                                printf("[Client] Started Panning\n");
                            }
                            // CHECK CTRL KEY STATE FOR LAYER MOVE
                            else {
                                const Uint8* state = SDL_GetKeyboardState(NULL);
                                if (state[SDL_SCANCODE_LCTRL] || state[SDL_SCANCODE_RCTRL]) {
                                    if (currentLayerId > 0) {
                                        clear_redo_stack();
                                        isMovingLayer = true;
                                        totalMoveX = 0;
                                        totalMoveY = 0;
                                        lastMouseX = mx;
                                        lastMouseY = my;
                                        printf("[Client] Started Layer Move\n");
                                    }
                                }
                                // If eyedropper is active, pick color
                                else if (isEyedropping) {
                                    // Transform to Canvas Coords
                                    int cx = mx - viewOffsetX;
                                    int cy = my - viewOffsetY;
                                    if (cx >= 0 && cx < CANVAS_WIDTH && cy >= 0 && cy < CANVAS_HEIGHT) {
                                        userColor = get_composite_pixel(cx, cy);
                                        isEyedropping = false; // Turn off after picking
                                        printf("[Client][Tool] Picked color: %d,%d,%d\n", userColor.r, userColor.g, userColor.b);
                                    }
                                } else {
                                    // Drawing on canvas - save undo state before starting stroke
                                    if (!strokeInProgress) {
                                        clear_redo_stack();
                                        currentPaintCmd = new PaintCommand(currentLayerId, CANVAS_WIDTH, CANVAS_HEIGHT);
                                        currentPaintCmd->captureBefore();
                                        strokeInProgress = true;
                                    }
                                    mouseDown = 1;
                                    lastMouseX = mx;
                                    lastMouseY = my;
                                    lastStableAngle = -999; // Reset angle state (Wait for motion)
                                    
                                    // Initial pressure check
                                    int pressure = 255;
                                    if (use_raw_input) {
                                        float p = RawInput_GetPressure();
                                        if (p >= 0.0f) pressure = (int)(p * 255);
                                    }
                                    
                                    // NOTE: We do NOT draw immediately here.
                                    // We wait for the first motion to determine angle.
                                    // If no motion occurs (just a click), we handle it in MOUSEBUTTONUP.
                                }
                            }
                        }
                    }
                }
                // Tablet support: Button 2 (Right Click) -> Eyedropper (Hold & Pick)
                else if (e.button.button == SDL_BUTTON_RIGHT) {
                    if (loggedin) {
                        isEyedropping = true;
                        // Immediate pick on press
                        int mx = e.button.x;
                        int my = e.button.y;
                        if (mx >= 0 && mx < CANVAS_WIDTH && my >= 0 && my < CANVAS_HEIGHT) {
                            userColor = get_composite_pixel(mx, my);
                            printf("[Client][Tool] Right-click picked color: %d,%d,%d\n", userColor.r, userColor.g, userColor.b);
                        }
                    }
                }
                // Tablet support: Button 3 (Middle Click) -> Undo
                else if (e.button.button == SDL_BUTTON_MIDDLE) {
                    if (loggedin) perform_undo();
                }
                break;

            case SDL_MOUSEBUTTONUP:
                // Ignore mouse up simulated from touch
                if (e.button.which == SDL_TOUCH_MOUSEID) {
                    break;
                }

                if (e.button.button == SDL_BUTTON_LEFT) {
                    if (isPanning) {
                        isPanning = false;
                        printf("[Client] Stopped Panning\n");
                    }
                    else if (isMovingLayer) {
                        // Finish Move: Send Command to Server
                        printf("[Client] Finished Move. Total Delta: (%d, %d)\n", totalMoveX, totalMoveY);
                        
                        if (totalMoveX != 0 || totalMoveY != 0) {
                            send_tcp_layer_move(currentLayerId, totalMoveX, totalMoveY);
                            
                            MoveCommand* cmd = new MoveCommand(currentLayerId, totalMoveX, totalMoveY);
                            push_undo_command(cmd);
                        }
                        
                        isMovingLayer = false;
                        totalMoveX = 0;
                        totalMoveY = 0;
                    }
                    else if (dragLayerId != -1) {
                        handle_drag_end(e.button.x, e.button.y);
                    }
                    
                    if (mouseDown) {
                        // If we never moved enough to establish an angle, draw a single dot now
                        if (lastStableAngle == -999 && lastMouseX != -1) {
                            int pressure = 255; // Default pressure for click
                            send_udp_draw(lastMouseX - viewOffsetX, lastMouseY - viewOffsetY, pressure, 0);
                        }

                        if (currentPaintCmd) {
                            currentPaintCmd->captureAfter();
                            push_undo_command(currentPaintCmd);
                            currentPaintCmd = nullptr;
                        }
                        strokeInProgress = false;
                    }
                    mouseDown = 0;
                    lastMouseX = -1;
                    lastMouseY = -1;
                    
                    if (isDrawingSignature) {
                        isDrawingSignature = false;
                        lastSigX = -1;
                        lastSigY = -1;
                    }
                }
                // Tablet support: Release Button 2 -> Stop Eyedropper
                else if (e.button.button == SDL_BUTTON_RIGHT) {
                    if (loggedin) isEyedropping = false;
                }
                break;

            case SDL_FINGERDOWN:
            case SDL_FINGERMOTION:
                // Handle touch events which carry pressure data
                if (loggedin) {
                    int mx = (int)(e.tfinger.x * CANVAS_WIDTH);
                    int my = (int)(e.tfinger.y * CANVAS_HEIGHT);
                    float pressure = e.tfinger.pressure; // 0.0 to 1.0
                    int pressureInt = (int)(pressure * 255);
                    
                    // Debug pressure
                    printf("[Client][Input] Finger Event: Type=%s, X=%d, Y=%d, Pressure=%.2f, DeviceID=%ld\n", 
                        (e.type == SDL_FINGERDOWN) ? "DOWN" : "MOTION", mx, my, pressure, (long)e.tfinger.touchId);
                    
                    // If finger down, start stroke
                    if (e.type == SDL_FINGERDOWN) {
                        if (!strokeInProgress) {
                            clear_redo_stack();
                            currentPaintCmd = new PaintCommand(currentLayerId, CANVAS_WIDTH, CANVAS_HEIGHT);
                            currentPaintCmd->captureBefore();
                            strokeInProgress = true;
                        }
                        mouseDown = 1; // Treat as mouse down
                        lastMouseX = mx;
                        lastMouseY = my;
                        lastStableAngle = -999; // Reset angle
                    }
                    
                    send_udp_cursor(mx - viewOffsetX, my - viewOffsetY);
                    
                    if (mouseDown && e.type != SDL_FINGERDOWN) {
                        // Interpolate
                        int angle = lastStableAngle;
                        if (lastMouseX >= 0 && lastMouseY >= 0) {
                            int dx = mx - lastMouseX;
                            int dy = my - lastMouseY;
                            float dist = sqrt(dx*dx + dy*dy);

                            if (lastStableAngle == -999) {
                                if (dist > 3.0f) {
                                    lastStableAngle = (int)(atan2(dy, dx) * 180.0 / M_PI);
                                    angle = lastStableAngle;
                                } else {
                                    break; // Pause
                                }
                            } else {
                                if (dist > 3.0f) {
                                    lastStableAngle = (int)(atan2(dy, dx) * 180.0 / M_PI);
                                    angle = lastStableAngle;
                                }
                            }
                            
                            int steps = max(abs(dx), abs(dy));
                            if (steps > 0) {
                                for (int i = 1; i <= steps; i++) {
                                    int ix = lastMouseX + (dx * i) / steps;
                                    int iy = lastMouseY + (dy * i) / steps;
                                    send_udp_draw(ix - viewOffsetX, iy - viewOffsetY, pressureInt, angle);
                                }
                            }
                        }
                        lastMouseX = mx;
                        lastMouseY = my;
                        // Also draw at current position
                        send_udp_draw(mx - viewOffsetX, my - viewOffsetY, pressureInt, angle);
                    }
                }
                break;

            case SDL_FINGERUP:
                if (mouseDown) {
                    // If we never moved enough to establish an angle, draw a single dot now
                    if (lastStableAngle == -999 && lastMouseX != -1) {
                        int pressure = 255; // Default pressure
                        send_udp_draw(lastMouseX - viewOffsetX, lastMouseY - viewOffsetY, pressure, 0);
                    }

                    if (currentPaintCmd) {
                        currentPaintCmd->captureAfter();
                        push_undo_command(currentPaintCmd);
                        currentPaintCmd = nullptr;
                    }
                    strokeInProgress = false;
                }
                mouseDown = 0;
                lastMouseX = -1;
                lastMouseY = -1;
                break;

            case SDL_MOUSEMOTION:
                // Ignore mouse motion simulated from touch
                if (e.motion.which == SDL_TOUCH_MOUSEID) {
                    break;
                }
                
                if (!loggedin && isDrawingSignature) {
                    int mx = e.motion.x;
                    int my = e.motion.y;
                    
                    // Map to texture space
                    int tx = mx - signatureRect.x;
                    int ty = my - signatureRect.y;
                    
                    if (mouseDown) {
                        SDL_SetRenderTarget(renderer, signatureTexture);
                        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255); // Black ink
                        
                        if (lastSigX >= 0 && lastSigY >= 0) {
                            // Draw thicker line (7x7 brush)
                            for (int w = -3; w <= 3; w++) {
                                for (int h = -3; h <= 3; h++) {
                                    SDL_RenderDrawLine(renderer, lastSigX + w, lastSigY + h, tx + w, ty + h);
                                }
                            }
                        } else {
                            // Draw point
                            for (int w = -3; w <= 3; w++) {
                                for (int h = -3; h <= 3; h++) {
                                    SDL_RenderDrawPoint(renderer, tx + w, ty + h);
                                }
                            }
                        }
                        
                        SDL_SetRenderTarget(renderer, NULL);
                        lastSigX = tx;
                        lastSigY = ty;
                    }
                }

                if (loggedin) {
                    int mx = e.motion.x;
                    int my = e.motion.y;
                    
                    if (isPanning) {
                        int dx = mx - lastMouseX;
                        int dy = my - lastMouseY;
                        viewOffsetX += dx;
                        viewOffsetY += dy;
                        lastMouseX = mx;
                        lastMouseY = my;
                    }
                    else if (isMovingLayer) {
                        int dx = mx - lastMouseX;
                        int dy = my - lastMouseY;
                        
                        if (dx != 0 || dy != 0) {
                            // 1. Move Locally (Visual Feedback)
                            move_layer_local(currentLayerId, dx, dy);
                            
                            // 2. Accumulate Total Move (For network sync)
                            totalMoveX += dx;
                            totalMoveY += dy;
                            
                            lastMouseX = mx;
                            lastMouseY = my;
                        }
                    }
                    
                    // --- CURSOR VISIBILITY LOGIC ---
                    bool showSystemCursor = false;
                    
                    // 1. Always show if UI is visible and we are hovering over it
                    if (uiVisible) {
                        // Check buttons (skip login buttons 0-2)
                        for (size_t i = 3; i < buttons.size(); i++) {
                            if (point_in_button(buttons[i], mx, my)) {
                                showSystemCursor = true;
                                break;
                            }
                        }
                    }
                    
                    // Apply state
                    if (showSystemCursor) {
                        SDL_ShowCursor(SDL_ENABLE);
                    } else {
                        SDL_ShowCursor(SDL_DISABLE);
                    }
                    
                    if (dragLayerId != -1) {
                        dragCurrentY = my;
                    }
                    
                    // Continuous Eyedropper Update
                    if (isEyedropping) {
                        // Transform to Canvas Coords
                        int cx = mx - viewOffsetX;
                        int cy = my - viewOffsetY;
                        if (cx >= 0 && cx < CANVAS_WIDTH && cy >= 0 && cy < CANVAS_HEIGHT) {
                            userColor = get_composite_pixel(cx, cy);
                        }
                    }
                    
                    if (loggedin && myUserId > 0) { // Only send if we know who we are
                        if (mx != lastSentX || my != lastSentY) {
                            // Send Canvas Coords
                            send_udp_cursor(mx - viewOffsetX, my - viewOffsetY);
                            lastSentX = mx;
                            lastSentY = my;
                        }
                    }
                    
                    if (mouseDown && !isPanning) {
                        // Standard mouse drawing (no pressure)
                        int pressure = 255;

                        if (use_raw_input) {
                            float p = RawInput_GetPressure();
                            if (p >= 0.0f) {
                                pressure = (int)(p * 255);
                                // printf("[Client][Input] Raw Pressure: %.2f -> %d\n", p, pressure);
                            }
                        }
                        
                        if (lastMouseX >= 0 && lastMouseY >= 0) {
                            int dx = mx - lastMouseX;
                            int dy = my - lastMouseY;
                            
                            // DISTANCE CHECK: Calculate how far we moved
                            float dist = sqrt(dx*dx + dy*dy);
                            
                            // PAUSE LOGIC: If we haven't established a direction yet...
                            if (lastStableAngle == -999) {
                                if (dist > 3.0f) {
                                    // We moved enough! Set the angle.
                                    lastStableAngle = (int)(atan2(dy, dx) * 180.0 / M_PI);
                                } else {
                                    // Not enough movement yet. Wait.
                                    // Do NOT update lastMouseX, so we draw from the start point later.
                                    break; 
                                }
                            } else {
                                // Normal update
                                if (dist > 3.0f) {
                                    lastStableAngle = (int)(atan2(dy, dx) * 180.0 / M_PI);
                                }
                            }
                            
                            // Use the stabilized angle
                            int angle = lastStableAngle;
                            
                            // Interpolate between last position and current
                            int steps = max(abs(dx), abs(dy));
                            if (steps > 0) {
                                for (int i = 1; i <= steps; i++) {
                                    int ix = lastMouseX + (dx * i) / steps;
                                    int iy = lastMouseY + (dy * i) / steps;
                                    // Transform to Canvas Coords
                                    send_udp_draw(ix - viewOffsetX, iy - viewOffsetY, pressure, angle);
                                }
                            }
                            // Also draw at current position
                            lastMouseX = mx;
                            lastMouseY = my;
                            send_udp_draw(mx - viewOffsetX, my - viewOffsetY, pressure, angle);
                        } else {
                            lastMouseX = mx;
                            lastMouseY = my;
                            // Don't draw if we don't have a previous point (shouldn't happen with new logic)
                        }
                    }
                }
                break;

            case SDL_CONTROLLERAXISMOTION:
                // Debugging: Check if tablet pressure is being sent as a joystick axis
                // printf("[Client][Input] Axis Motion: Axis %d Value %d\n", e.caxis.axis, e.caxis.value);
                break;

            case SDL_JOYAXISMOTION:
                // Debugging: Check if tablet pressure is being sent as a joystick axis
                // printf("[Client][Input] Joystick Axis: Axis %d Value %d\n", e.jaxis.axis, e.jaxis.value);
                break;

            case SDL_KEYDOWN:
                if (e.key.keysym.sym == SDLK_SPACE) spacePressed = true;

                // --- CANVAS SELECTION (When in Menu) ---
                if (!loggedin) {
                    // Allow typing numbers 0-9 to change canvas ID
                    if (e.key.keysym.sym >= SDLK_0 && e.key.keysym.sym <= SDLK_9) {
                        int digit = e.key.keysym.sym - SDLK_0;
                        // Shift left and add new digit, mod 100
                        currentCanvasId = (currentCanvasId * 10 + digit) % 100;
                        printf("[Client][Lobby] Canvas selection: %02d\n", currentCanvasId);
                    }
                }

                if (loggedin) {
                    if (e.key.keysym.sym == SDLK_TAB) {
                        uiVisible = !uiVisible;
                        printf("[Client] UI Visibility: %s\n", uiVisible ? "ON" : "OFF");
                    }
                    else if (e.key.keysym.sym == SDLK_q) {
                        if (currentBrushId >= 0 && currentBrushId < (int)availableBrushes.size()) {
                            if (availableBrushes[currentBrushId]->size > 1) {
                                availableBrushes[currentBrushId]->size--;
                                printf("[Client][UI] Brush size decreased to %d\n", availableBrushes[currentBrushId]->size);
                            }
                        }
                    } else if (e.key.keysym.sym == SDLK_w) {
                        if (currentBrushId >= 0 && currentBrushId < (int)availableBrushes.size()) {
                            if (availableBrushes[currentBrushId]->size < 150) {
                                availableBrushes[currentBrushId]->size++;
                                printf("[Client][UI] Brush size increased to %d\n", availableBrushes[currentBrushId]->size);
                            }
                        }
                    }
                }
                switch (e.key.keysym.sym) {
                    case SDLK_ESCAPE:
                        if (loggedin) {
                            printf("[Client] Logging out...\n");
                            
                            // --- FIX: FORCE CURSOR VISIBLE ---
                            SDL_ShowCursor(SDL_ENABLE);

                            // 1. Disconnect Network
                            loggedin = 0; // Set FIRST so thread knows it's voluntary
                            
                            // Closing the socket will cause the tcp_receiver_thread to exit naturally
                            if (tcpSock >= 0) close(tcpSock);
                            if (udpSock >= 0) close(udpSock);
                            tcpSock = -1;
                            udpSock = -1;
                            
                            tcp_receiver_tid = 0; // Reset thread ID so we can spawn a new one on next login
                            
                            // 2. Reset State
                            myUserId = 0;
                            
                            // 3. Reset Layers (Clean Slate)
                            pthread_mutex_lock(&layerMutex);
                            for (int i = 0; i < MAX_LAYERS; i++) {
                                if (layers[i]) { delete[] layers[i]; layers[i] = nullptr; }
                                if (layerTextures[i]) { SDL_DestroyTexture(layerTextures[i]); layerTextures[i] = nullptr; }
                                layerIsDirty[i] = false;
                            }
                            // Re-init canvas defaults (Paper + 1 Transparent)
                            init_canvas(); 
                            currentLayerId = 1;
                            
                            // 4. Clear Remote State
                            pthread_mutex_lock(&remoteClientsMutex);
                            for (auto& [uid, client] : remoteClients) {
                                if (client.sigTexture) SDL_DestroyTexture(client.sigTexture);
                            }
                            remoteClients.clear();
                            remote_cursors.clear();
                            pthread_mutex_unlock(&remoteClientsMutex);
                            
                            // 5. Reset UI (Remove layer buttons)
                            UpdateLayerButtons();
                            pthread_mutex_unlock(&layerMutex);
                            
                            // --- RESIZE WINDOW TO MENU MODE ---
                            SDL_SetWindowSize(window, MENU_WIDTH, MENU_HEIGHT);
                            SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
                            windowWidth = MENU_WIDTH;
                            windowHeight = MENU_HEIGHT;
                            SetupUI();
                            
                            printf("[Client] Logged out successfully.\n");
                        } else {
                            // If in menu, Quit Application
                            running = 0;
                        }
                        break;
                    case SDLK_s:
                        if (e.key.keysym.mod & KMOD_CTRL) {
                            send_tcp_save();
                        } else {
                            // Increase opacity
                            if (currentBrushId >= 0 && currentBrushId < (int)availableBrushes.size()) {
                                int op = availableBrushes[currentBrushId]->opacity;
                                op = min(255, op + 10);
                                availableBrushes[currentBrushId]->opacity = op;
                                printf("[Client][Brush] Opacity increased to %d\n", op);
                            }
                        }
                        break;
                    case SDLK_a:
                        // Decrease opacity
                        if (currentBrushId >= 0 && currentBrushId < (int)availableBrushes.size()) {
                            int op = availableBrushes[currentBrushId]->opacity;
                            op = max(0, op - 10);
                            availableBrushes[currentBrushId]->opacity = op;
                            printf("[Client][Brush] Opacity decreased to %d\n", op);
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
                    case SDLK_4:
                        currentBrushId = 3;
                        break;
                    case SDLK_5:
                        currentBrushId = 4;
                        break;
                    case SDLK_6:
                        currentBrushId = 5;
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
                    case SDLK_LEFT:
                        if (currentLayerId > 0 && currentLayerId < MAX_LAYERS) {
                            layerOpacity[currentLayerId] = max(0, layerOpacity[currentLayerId] - 25);
                            printf("[Client][Layer] Layer %d opacity: %d\n", currentLayerId, layerOpacity[currentLayerId]);
                        }
                        break;
                    case SDLK_RIGHT:
                        if (currentLayerId > 0 && currentLayerId < MAX_LAYERS) {
                            layerOpacity[currentLayerId] = min(255, layerOpacity[currentLayerId] + 25);
                            printf("[Client][Layer] Layer %d opacity: %d\n", currentLayerId, layerOpacity[currentLayerId]);
                        }
                        break;
                    default:
                        break;
                }
                break;

            case SDL_KEYUP:
                if (e.key.keysym.sym == SDLK_SPACE) spacePressed = false;
                break;

            default:
                break;
        }
    }
}



/*****************************************************************************
   MAIN
 *****************************************************************************/

void process_dirty_updates() {
    pthread_mutex_lock(&layerMutex); // Lock to prevent tearing

    for (int i = 0; i < MAX_LAYERS; i++) {
        if (layerIsDirty[i] && layerTextures[i]) {
            SDL_Rect* r = &layerDirtyRects[i];
            
            // Convert Min/Max storage back to X/Y/W/H for SDL
            // Current: x=minX, y=minY, w=maxX, h=maxY
            int realW = r->w - r->x;
            int realH = r->h - r->y;

            if (realW > 0 && realH > 0) {
                SDL_Rect updateRect = {r->x, r->y, realW, realH};
                
                // Calculate memory offset
                // Pixels are stored as 1D array, we point to the top-left of the dirty rect
                uint8_t* pixelStart = layers[i] + (r->y * CANVAS_WIDTH + r->x) * 4;
                
                SDL_UpdateTexture(layerTextures[i], &updateRect, pixelStart, CANVAS_WIDTH * 4);
            }

            // Reset for next frame
            layerDirtyRects[i] = {CANVAS_WIDTH, CANVAS_HEIGHT, 0, 0};
            layerIsDirty[i] = false;
        }
    }

    pthread_mutex_unlock(&layerMutex);
}

int main(int argc, char* argv[]) {
    init_dirty_rects();
    // Prevent crash when writing to closed socket
    signal(SIGPIPE, SIG_IGN);

    printf("[Client][Main] ==============================================\n");
    printf("[Client][Main] Shared Canvas Client Starting\n");
    printf("[Client][Main] ==============================================\n");
    
    // Parse command line
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--nuclear") == 0) {
            use_raw_input = true;
            printf("[Client][Main] NUCLEAR OPTION ENABLED: Using raw input for pressure\n");
        } else if (argv[i][0] != '-') {
            strncpy(serverIp, argv[i], sizeof(serverIp) - 1);
        }
    }
    printf("[Client][Main] Server IP: %s\n", serverIp);

    // Initialize Raw Input if requested
    if (use_raw_input) {
        if (!RawInput_Start()) {
            printf("[Client][Main] Failed to start Nuclear Input. Falling back to SDL.\n");
            use_raw_input = false;
        }
    }

    // Initialize SDL
    printf("[Client][Main] Initializing SDL...\n");
    // Hint to separate mouse and touch events (helps with Wacom pressure)
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        printf("[Client][Main] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    window = SDL_CreateWindow("Shared Canvas",
                              SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              MENU_WIDTH, MENU_HEIGHT,
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

    // Create Signature Texture
    signatureTexture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
                                         SDL_TEXTUREACCESS_TARGET,
                                         SIGNATURE_WIDTH, SIGNATURE_HEIGHT);
    if (!signatureTexture) {
        printf("[Client][Main] Failed to create signature texture: %s\n", SDL_GetError());
    } else {
        // Clear it to transparent
        SDL_SetTextureBlendMode(signatureTexture, SDL_BLENDMODE_BLEND);
        SDL_SetRenderTarget(renderer, signatureTexture);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 0); // Transparent
        SDL_RenderClear(renderer);
        SDL_SetRenderTarget(renderer, NULL);
    }

    printf("[Client][Main] SDL initialized successfully\n");
    
    SDL_version compiled;
    SDL_version linked;
    SDL_VERSION(&compiled);
    SDL_GetVersion(&linked);
    printf("[Client][Debug] SDL Compiled Version: %d.%d.%d\n", compiled.major, compiled.minor, compiled.patch);
    printf("[Client][Debug] SDL Linked Version: %d.%d.%d\n", linked.major, linked.minor, linked.patch);

    // Debug: Print Video Driver
    const char* videoDriver = SDL_GetCurrentVideoDriver();
    printf("[Client][Debug] Current Video Driver: %s\n", videoDriver ? videoDriver : "Unknown");
    
    // Debug: Print Touch Devices
    int numTouchDevices = SDL_GetNumTouchDevices();
    printf("[Client][Debug] Number of Touch Devices: %d\n", numTouchDevices);
    for (int i = 0; i < numTouchDevices; i++) {
        SDL_TouchID touchId = SDL_GetTouchDevice(i);
        printf("[Client][Debug] Touch Device %d ID: %ld\n", i, (long)touchId);
    }

    // Initialize systems
    load_menu_ui();
    update_menu_texture();
    init_canvas();
    init_brushes();

    // --- FIX 1: Move Left (160) and Standardize Size (300x150) ---
    signatureRect.x = 95; // Centered: (640 - 450) / 2 = 95
    signatureRect.y = 140; 
    signatureRect.w = 450; // Fixed width
    signatureRect.h = 150;

    // --- FIX 2: Re-create Texture to MATCH the Rect Size ---
    // This prevents the "drawing offset" caused by scaling
    if (signatureTexture) SDL_DestroyTexture(signatureTexture);
    signatureTexture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, 
                                       SDL_TEXTUREACCESS_TARGET, 
                                       signatureRect.w, signatureRect.h);
    // Set Blend Mode so it doesn't turn black
    SDL_SetTextureBlendMode(signatureTexture, SDL_BLENDMODE_BLEND);

    // Clear it to transparent immediately
    SDL_SetRenderTarget(renderer, signatureTexture);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
    SDL_RenderClear(renderer);
    SDL_SetRenderTarget(renderer, NULL);

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
        // Animation Logic for Menu
        uint32_t now = SDL_GetTicks();
        if (!loggedin && now - lastMenuAnimTime > 1000) { // Switch every 1000ms (1 second)
            currentMenuFrame = !currentMenuFrame; // Toggle 0 <-> 1
            update_menu_texture();
            lastMenuAnimTime = now;
        }

        handle_events();
        
        // Polling for pressure changes (Nuclear Option)
        // This ensures we catch pressure drops even if the mouse doesn't move (e.g. lifting pen)
        if (use_raw_input && mouseDown && loggedin && !isEyedropping) {
            float p = RawInput_GetPressure();
            int pressure = (int)(p * 255);
            
            // If pressure changed significantly or dropped to zero, send update
            if (abs(pressure - lastSentPressure) > 2 || (pressure == 0 && lastSentPressure > 0)) {
                // Use last known mouse position
                if (lastMouseX >= 0 && lastMouseY >= 0) {
                    send_udp_draw(lastMouseX, lastMouseY, pressure);
                }
            }
        }


        
        // Check for pending layer updates from TCP thread
        if (pendingLayerUpdate) {
            pendingLayerUpdate = false;
            UpdateLayerButtons();
        }

        // --- SIGNATURE IMPLEMENTATION START ---
        // Check for pending signatures
        if (pendingSigUpdate) {
            pthread_mutex_lock(&sigMutex);
            std::vector<PendingSig> toProcess = pendingSignatures;
            pendingSignatures.clear();
            pendingSigUpdate = false;
            pthread_mutex_unlock(&sigMutex);
            
            for (const auto& ps : toProcess) {
                printf("[Client][Main] Processing pending signature for UID=%d...\n", ps.user_id);
                // Reconstruct 45x15 surface (from 10x10 blocks)
                SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, 45, 15, 32, SDL_PIXELFORMAT_RGBA8888);
                if (surf) {
                    SDL_LockSurface(surf);
                    // Clear to transparent
                    memset(surf->pixels, 0, 45 * 15 * 4);
                    
                    int setPixels = 0;
                    for (int i = 0; i < 256; i++) {
                        uint8_t byte = ps.data[i];
                        // 4 pixels per byte (2 bits each)
                        for (int p = 0; p < 4; p++) {
                            int shift = (3 - p) * 2;
                            uint8_t val = (byte >> shift) & 0x03;
                            
                            if (val > 0) {
                                int pixelIdx = i * 4 + p;
                                int x = pixelIdx % 45;
                                int y = pixelIdx / 45;
                                
                                if (x < 45 && y < 15) {
                                    uint8_t alpha = val * 85;
                                    Uint32 color = SDL_MapRGBA(surf->format, 255, 255, 255, alpha);
                                    ((Uint32*)surf->pixels)[y * 45 + x] = color;
                                    setPixels++;
                                }
                            }
                        }
                    }
                    SDL_UnlockSurface(surf);
                    
                    // Store in remoteClients map
                    pthread_mutex_lock(&remoteClientsMutex);
                    if (remoteClients[ps.user_id].sigTexture) {
                        SDL_DestroyTexture(remoteClients[ps.user_id].sigTexture);
                    }
                    remoteClients[ps.user_id].sigTexture = SDL_CreateTextureFromSurface(renderer, surf);
                    SDL_SetTextureBlendMode(remoteClients[ps.user_id].sigTexture, SDL_BLENDMODE_BLEND);
                    remoteClients[ps.user_id].hasSignature = true;
                    pthread_mutex_unlock(&remoteClientsMutex);
                    
                    SDL_FreeSurface(surf);
                    printf("[Client][Main] Stored signature for UID=%d\n", ps.user_id);
                }
            }
        }
        // --- SIGNATURE IMPLEMENTATION END ---

        update_canvas_texture();
        
        // --- NEW: UPLOAD TEXTURES TO GPU HERE ---
        // This runs EXACTLY ONCE per frame, no matter how many brush steps happened.
        process_dirty_updates(); 

        draw_ui(renderer, uiVisible, [&](SDL_Renderer* r) {
            // Render remote signatures attached to cursors
            pthread_mutex_lock(&remoteClientsMutex);
            for (auto& [uid, client] : remoteClients) {
                if (uid == myUserId) continue; // Hide own signature
                // if (client.hasSignature) printf("[Client][Render] UID=%d hasSig=%d tex=%p pos=(%d,%d)\n", uid, client.hasSignature, client.sigTexture, client.x, client.y);
                if (client.hasSignature && client.sigTexture) {
                    // 1. Apply the user's color to the signature texture
                    SDL_SetTextureColorMod(client.sigTexture, client.r, client.g, client.b);

                    // 2. Smaller size (e.g., 80x26 instead of 120x40)
                    // 3. Positioned to bottom-right (Offset x+15, y+15)
                    //SIGNATURE POSITIONING
                    SDL_Rect sigRect = {
                        client.x, 
                        client.y, 
                        80, 26 
                    };
                    SDL_RenderCopy(r, client.sigTexture, NULL, &sigRect);
                }
            }
            pthread_mutex_unlock(&remoteClientsMutex);

            // Draw Eyedropper Crosshair
            if (isEyedropping) {
                int mx, my;
                SDL_GetMouseState(&mx, &my);
                
                // Draw crosshair (Black outline, White inner)
                SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
                SDL_RenderDrawLine(r, mx - 6, my, mx + 6, my);
                SDL_RenderDrawLine(r, mx, my - 6, mx, my + 6);
                
                SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
                SDL_RenderDrawLine(r, mx - 3, my, mx + 3, my);
                SDL_RenderDrawLine(r, mx, my - 3, mx, my + 3);
            }
        });
        
        SDL_Delay(16); // ~60 FPS
    }

    printf("[Client][Main] Shutting down...\n");

    if (use_raw_input) {
        RawInput_Stop();
    }

    // Cleanup
    if (tcpSock >= 0) close(tcpSock);
    if (udpSock >= 0) close(udpSock);
    
    for (auto* brush : availableBrushes) delete brush;
    for (auto* btn : buttons) delete btn;
    
    // Cleanup undo/redo stacks
    for (auto* cmd : undoStack) delete cmd;
    for (auto* cmd : redoStack) delete cmd;
    undoStack.clear();
    redoStack.clear();
    
    // Cleanup layer textures
    for (int i = 0; i < MAX_LAYERS; i++) {
        if (layerTextures[i]) SDL_DestroyTexture(layerTextures[i]);
        if (layers[i]) delete[] layers[i];
    }

    if (compositeCanvas) delete[] compositeCanvas;
    if (canvasTexture) SDL_DestroyTexture(canvasTexture);
    if (signatureTexture) SDL_DestroyTexture(signatureTexture);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);

    SDL_Quit();
    return 0;
}
