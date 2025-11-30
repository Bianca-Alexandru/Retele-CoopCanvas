/* 
   TCP/UDP Hybrid Client - Shared Canvas
   
   Protocol Scheme:
   UDP: DRAW, CURSOR, UNDO, REDO
   TCP: LOGIN, CONNECT, COLOR, BRUSH
*/

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <SDL2/SDL.h>
#include <vector>
#include <errno.h>
#include <map>
#include <string>
#include "brushes.h"
using namespace std;

#define WIDTH 640
#define HEIGHT 480

struct RemoteCursor {
    int x, y;
    SDL_Color color;
};
map<string, RemoteCursor> remote_cursors;

SDL_Color userColor = {0, 0, 0, 255};
bool isDrawing = false;
int last_draw_x = -1;
int last_draw_y = -1;
int currentBrushId = 0;
int loggedin = 0;
vector<Brush*> availableBrushes;

void send_tcp_login(const char* username);

class Button {
public:
    int x, y, w, h;
    SDL_Color color;
    virtual void Draw(SDL_Renderer* renderer) = 0;
    virtual void Click() = 0;
};

vector<Button*> buttons;

class ColorPicker: public Button {
public:
    virtual void Draw(SDL_Renderer* renderer) override {
        for(int i = 0; i < w; i++) {
            for(int j = 0; j < h; j++) {
                // Saturation (Left -> Right): White -> Base Color
                float sat = (float)i / w;
                int r_s = 255 + (color.r - 255) * sat;
                int g_s = 255 + (color.g - 255) * sat;
                int b_s = 255 + (color.b - 255) * sat;

                // Value (Top -> Bottom): Bright -> Dark
                float val = 1.0f - (float)j / h;
                
                SDL_SetRenderDrawColor(renderer, 
                    (Uint8)(r_s * val), 
                    (Uint8)(g_s * val), 
                    (Uint8)(b_s * val), 
                    255);
                SDL_RenderDrawPoint(renderer, x + i, y + j);
            }
        }
    }

    virtual void Click() override {
        int mx, my;
        SDL_GetMouseState(&mx, &my);
        
        // Relative coordinates clamped to the button area
        int rel_x = mx - x;
        int rel_y = my - y;
        if(rel_x < 0) rel_x = 0; if(rel_x >= w) rel_x = w - 1;
        if(rel_y < 0) rel_y = 0; if(rel_y >= h) rel_y = h - 1;

        // Re-calculate color at this position
        float sat = (float)rel_x / w;
        int r_s = 255 + (color.r - 255) * sat;
        int g_s = 255 + (color.g - 255) * sat;
        int b_s = 255 + (color.b - 255) * sat;

        float val = 1.0f - (float)rel_y / h;
        
        userColor.r = (Uint8)(r_s * val);
        userColor.g = (Uint8)(g_s * val);
        userColor.b = (Uint8)(b_s * val);
        userColor.a = 255;
    }
};
class HuePicker: public Button {
public:
    ColorPicker* linkedPicker;

    virtual void Draw(SDL_Renderer* renderer) override {
        for(int i = 0; i < w; i++) {
            float hue = (float)i / w * 6.0f;
            float x_val = 1 - fabs(fmod(hue, 2) - 1);
            int r = 0, g = 0, b = 0;
            
            if(hue < 1) { r=255; g=x_val*255; b=0; }
            else if(hue < 2) { r=x_val*255; g=255; b=0; }
            else if(hue < 3) { r=0; g=255; b=x_val*255; }
            else if(hue < 4) { r=0; g=x_val*255; b=255; }
            else if(hue < 5) { r=x_val*255; g=0; b=255; }
            else { r=255; g=0; b=x_val*255; }

            SDL_SetRenderDrawColor(renderer, r, g, b, 255);
            SDL_RenderDrawLine(renderer, x + i, y, x + i, y + h);
        }
    }

    virtual void Click() override {
        int mx, my;
        SDL_GetMouseState(&mx, &my);
        int rel_x = mx - x;
        if(rel_x < 0) rel_x = 0; if(rel_x >= w) rel_x = w - 1;

        float hue = (float)rel_x / w * 6.0f;
        float x_val = 1 - fabs(fmod(hue, 2) - 1);
        int r = 0, g = 0, b = 0;
        
        if(hue < 1) { r=255; g=x_val*255; b=0; }
        else if(hue < 2) { r=x_val*255; g=255; b=0; }
        else if(hue < 3) { r=0; g=255; b=x_val*255; }
        else if(hue < 4) { r=0; g=x_val*255; b=255; }
        else if(hue < 5) { r=x_val*255; g=0; b=255; }
        else { r=255; g=0; b=x_val*255; }

        if(linkedPicker) {
            linkedPicker->color = {(Uint8)r, (Uint8)g, (Uint8)b, 255};
        }
    }
};

class BrushButton : public Button {
public:
    int brushId;
    virtual void Draw(SDL_Renderer* renderer) override {
        // Draw background
        if (currentBrushId == brushId) {
            SDL_SetRenderDrawColor(renderer, 150, 150, 150, 255); // Selected
        } else {
            SDL_SetRenderDrawColor(renderer, 220, 220, 220, 255); // Normal
        }
        SDL_Rect rect = {x, y, w, h};
        SDL_RenderFillRect(renderer, &rect);
        
        // Draw border
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderDrawRect(renderer, &rect);

        // Draw Icon
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        if (brushId == 0) { // Round
             // Draw a small circle approximation
             for(int i=-3; i<=3; i++)
                for(int j=-3; j<=3; j++)
                    if(i*i+j*j <= 9) SDL_RenderDrawPoint(renderer, x+w/2+i, y+h/2+j);
        } else { // Square
             SDL_Rect r = {x + w/2 - 3, y + h/2 - 3, 7, 7};
             SDL_RenderFillRect(renderer, &r);
        }
    }
    virtual void Click() override {
        currentBrushId = brushId;
        printf("Brush switched to %d\n", brushId);
    }
};

class LoginButton : public Button {
public:
    virtual void Draw(SDL_Renderer* renderer) override {
        SDL_SetRenderDrawColor(renderer, 100, 200, 100, 255);
        SDL_Rect rect = {x, y, w, h};
        SDL_RenderFillRect(renderer, &rect);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderDrawRect(renderer, &rect);
        // Could add text rendering here
    }
    virtual void Click() override {
        send_tcp_login("User1"); // Example username
    }
};

class SizeUpButton : public Button { 
    public:
    virtual void Draw(SDL_Renderer* renderer) override {
        SDL_SetRenderDrawColor(renderer, 100, 200, 100, 255);
        SDL_Rect rect = {x, y, w, h};
        SDL_RenderFillRect(renderer, &rect);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderDrawRect(renderer, &rect);
        // Could add text rendering here
    }
    virtual void Click() override {
        if(currentBrushId >= 0 && currentBrushId < availableBrushes.size()) {
            availableBrushes[currentBrushId]->size++;
            printf("Brush size increased to %d\n", availableBrushes[currentBrushId]->size);
        }
    }
};
class SizeDownButton : public Button { 
    public:
    virtual void Draw(SDL_Renderer* renderer) override {
        SDL_SetRenderDrawColor(renderer, 200, 100, 100, 255);
        SDL_Rect rect = {x, y, w, h};
        SDL_RenderFillRect(renderer, &rect);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderDrawRect(renderer, &rect);
        // Could add text rendering here
    }
    virtual void Click() override {
        if(currentBrushId >= 0 && currentBrushId < availableBrushes.size()) {
            if(availableBrushes[currentBrushId]->size > 1)
                availableBrushes[currentBrushId]->size--;
            printf("Brush size decreased to %d\n", availableBrushes[currentBrushId]->size);
        }
    }
};


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

// UDP Message Structure
struct UDPMessage {
    int type;
    int x, y;
    int ex, ey;
    int id_brush;
    int size;
    SDL_Color color;
    float pressure;
    int layer;
};

// TCP Message Structure
struct TCPMessage {
    int type;
    char username[64];
    int id_canvas;
    SDL_Color color;
    int id_brush;
};

/* --- GLOBAL STATE --- */
int tcp_sd;
int udp_sd;
struct sockaddr_in server_addr_udp;
SDL_Texture* canvasTexture = NULL;

/* --- NETWORK HELPERS --- */

void setup_network(const char* ip, int port) {
    // 1. Setup TCP
    tcp_sd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in server_tcp;
    server_tcp.sin_family = AF_INET;
    server_tcp.sin_addr.s_addr = inet_addr(ip);
    server_tcp.sin_port = htons(port);
    
    if (connect(tcp_sd, (struct sockaddr *)&server_tcp, sizeof(server_tcp)) == -1) {
        perror("TCP Connect failed");
        exit(1);
    }

    // 2. Setup UDP
    udp_sd = socket(AF_INET, SOCK_DGRAM, 0);
    server_addr_udp.sin_family = AF_INET;
    server_addr_udp.sin_addr.s_addr = inet_addr(ip);
    server_addr_udp.sin_port = htons(port);
}

void send_udp_draw(int x, int y, SDL_Color color) {
    UDPMessage msg;
    msg.type = MSG_DRAW;
    msg.x = x;
    msg.y = y;
    msg.id_brush = currentBrushId;
    msg.size = availableBrushes[currentBrushId]->size;
    msg.color = color;
    msg.pressure = 1.0f;
    msg.layer = 0;
    
    sendto(udp_sd, &msg, sizeof(UDPMessage), 0, (struct sockaddr*)&server_addr_udp, sizeof(server_addr_udp));
}

void send_udp_cursor(int x, int y, SDL_Color color) {
    UDPMessage msg;
    msg.type = MSG_CURSOR;
    msg.x = x;
    msg.y = y;
    msg.color = color;
    
    sendto(udp_sd, &msg, sizeof(UDPMessage), 0, (struct sockaddr*)&server_addr_udp, sizeof(server_addr_udp));
}

void send_tcp_login(const char* username) {
    TCPMessage msg;
    msg.type = MSG_LOGIN;
    strncpy(msg.username, username, 63);
    write(tcp_sd, &msg, sizeof(TCPMessage));
}

void send_udp_line(int x1, int y1, int x2, int y2, SDL_Color color) {
    UDPMessage msg;
    msg.type = MSG_LINE;
    msg.x = x1;
    msg.y = y1;
    msg.ex = x2;
    msg.ey = y2;
    msg.id_brush = currentBrushId;
    msg.size = availableBrushes[currentBrushId]->size;
    msg.color = color;
    msg.pressure = 1.0f;
    msg.layer = 0;
    
    sendto(udp_sd, &msg, sizeof(UDPMessage), 0, (struct sockaddr*)&server_addr_udp, sizeof(server_addr_udp));
}

/* --- GRAPHICS HELPERS --- */

void draw_ui(SDL_Renderer* renderer) {
    // 1. Clear screen
    SDL_SetRenderDrawColor(renderer, 200, 200, 200, 255); // Grey background
    SDL_RenderClear(renderer);

    // 2. Draw Canvas Texture
    SDL_RenderCopy(renderer, canvasTexture, NULL, NULL);

    // 3. Draw UI Buttons
    for (Button* btn : buttons) {
        btn->Draw(renderer);
    }

    // 4. Draw Remote Cursors
    for (auto const& [key, cursor] : remote_cursors) {
        SDL_SetRenderDrawColor(renderer, cursor.color.r, cursor.color.g, cursor.color.b, 255);
        
        int size = 5;
        int gap = 2;
        
        // Left
        SDL_RenderDrawLine(renderer, cursor.x - size, cursor.y, cursor.x - gap, cursor.y);
        // Right
        SDL_RenderDrawLine(renderer, cursor.x + gap, cursor.y, cursor.x + size, cursor.y);
        // Top
        SDL_RenderDrawLine(renderer, cursor.x, cursor.y - size, cursor.x, cursor.y - gap);
        // Bottom
        SDL_RenderDrawLine(renderer, cursor.x, cursor.y + gap, cursor.x, cursor.y + size);
    }

    SDL_RenderPresent(renderer);
}

bool init_sdl(SDL_Window** window, SDL_Renderer** renderer) {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        printf("SDL Init failed: %s\n", SDL_GetError());
        return false;
    }

    *window = SDL_CreateWindow("Shared Canvas", 
                             SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 
                             WIDTH, HEIGHT, SDL_WINDOW_SHOWN);
    if (!*window) return false;

    *renderer = SDL_CreateRenderer(*window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE);
    if (!*renderer) return false;

    // Create persistent canvas texture
    canvasTexture = SDL_CreateTexture(*renderer, SDL_PIXELFORMAT_RGBA8888, 
                                    SDL_TEXTUREACCESS_TARGET, WIDTH, HEIGHT);
    
    // Clear texture to white
    SDL_SetRenderTarget(*renderer, canvasTexture);
    SDL_SetRenderDrawColor(*renderer, 255, 255, 255, 255);
    SDL_RenderClear(*renderer);
    SDL_SetRenderTarget(*renderer, NULL);

    return true;
}

void receive_initial_canvas(SDL_Renderer* renderer) {
    printf("[client] Syncing canvas...\n");
    size_t total_size = WIDTH * HEIGHT * sizeof(Pixel);
    Pixel *canvas_buffer = (Pixel *)malloc(total_size);
    
    size_t received = 0;
    char *ptr = (char *)canvas_buffer;
    while (received < total_size) {
        int r = read(tcp_sd, ptr + received, total_size - received);
        if (r <= 0) break;
        received += r;
    }

    // Render the buffer to the texture
    SDL_SetRenderTarget(renderer, canvasTexture);
    for (int x = 0; x < WIDTH; x++) {
        for (int y = 0; y < HEIGHT; y++) {
            Pixel (*view)[HEIGHT] = (Pixel (*)[HEIGHT])canvas_buffer;
            Pixel p = view[x][y];

            // Draw everything, not just non-white, to ensure sync
            SDL_SetRenderDrawColor(renderer, p.r, p.g, p.b, p.a);
            SDL_RenderDrawPoint(renderer, x, y);
        }
    }
    SDL_SetRenderTarget(renderer, NULL);
    free(canvas_buffer);
}

/* --- EVENT LOOP --- */

void handle_input(SDL_Renderer* renderer, bool* quit) {
    SDL_Event e;
    while (SDL_PollEvent(&e) != 0) {
        if (e.type == SDL_QUIT) {
            *quit = true;
        } else if (e.type == SDL_MOUSEBUTTONDOWN) {
            if (e.button.button == SDL_BUTTON_LEFT) {
                int x = e.button.x;
                int y = e.button.y;
                
                bool clicked_ui = false;
                for (Button* btn : buttons) {
                    if (x >= btn->x && x < btn->x + btn->w &&
                        y >= btn->y && y < btn->y + btn->h) {
                        btn->Click();
                        clicked_ui = true;
                    }
                }

                if (clicked_ui) {
                    // draw_ui(renderer); // Handled in main loop
                } else {
                    isDrawing = true;
                    last_draw_x = x;
                    last_draw_y = y;
                    
                    // Draw point on texture
                    SDL_SetRenderTarget(renderer, canvasTexture);
                    auto setPixel = [&](int px, int py, Pixel c) {
                        SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, c.a);
                        SDL_RenderDrawPoint(renderer, px, py);
                    };
                    if(currentBrushId >= 0 && currentBrushId < availableBrushes.size()) {
                        availableBrushes[currentBrushId]->paint(x, y, userColor, availableBrushes[currentBrushId]->size, setPixel);
                    }
                    SDL_SetRenderTarget(renderer, NULL);

                    // Send via UDP
                    send_udp_draw(x, y, userColor);
                }
            }
        } else if (e.type == SDL_MOUSEBUTTONUP) {
            if (e.button.button == SDL_BUTTON_LEFT) {
                isDrawing = false;
                last_draw_x = -1;
                last_draw_y = -1;
            }
        } else if (e.type == SDL_MOUSEMOTION) {
            int x = e.motion.x;
            int y = e.motion.y;
            
            // Send cursor position via UDP
            send_udp_cursor(x, y, userColor);

            if (isDrawing) {
                if (x != last_draw_x || y != last_draw_y) {
                    // Draw line on texture
                    SDL_SetRenderTarget(renderer, canvasTexture);
                    
                    // Interpolate locally with brush
                    int x0 = last_draw_x, y0 = last_draw_y;
                    int x1 = x, y1 = y;
                    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
                    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
                    int err = dx + dy, e2;
                    
                    auto setPixel = [&](int px, int py, Pixel c) {
                        SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, c.a);
                        SDL_RenderDrawPoint(renderer, px, py);
                    };

                    while (1) {
                        if(currentBrushId >= 0 && currentBrushId < availableBrushes.size()) {
                            availableBrushes[currentBrushId]->paint(x0, y0, userColor, availableBrushes[currentBrushId]->size, setPixel);
                        }
                        if (x0 == x1 && y0 == y1) break;
                        e2 = 2 * err;
                        if (e2 >= dy) { err += dy; x0 += sx; }
                        if (e2 <= dx) { err += dx; y0 += sy; }
                    }
                    
                    SDL_SetRenderTarget(renderer, NULL);

                    // Send Line via UDP
                    send_udp_line(last_draw_x, last_draw_y, x, y, userColor);
                    
                    last_draw_x = x;
                    last_draw_y = y;
                }
            }
        }
    }
}

void handle_network(SDL_Renderer* renderer, bool* quit) {
    fd_set readfds;
    struct timeval tv = {0, 0};
    
    FD_ZERO(&readfds);
    FD_SET(tcp_sd, &readfds);
    FD_SET(udp_sd, &readfds);
    
    int max_sd = (tcp_sd > udp_sd) ? tcp_sd : udp_sd;

    if (select(max_sd + 1, &readfds, NULL, NULL, &tv) > 0) {
        
        // Check TCP (Control messages)
        if (FD_ISSET(tcp_sd, &readfds)) {
            TCPMessage msg;
            int bytes = read(tcp_sd, &msg, sizeof(TCPMessage));
            if (bytes <= 0) {
                printf("[client] Server disconnected.\n");
                *quit = true;
            }
            // Handle TCP messages if needed
        }

        // Check UDP (Draw updates)
        if (FD_ISSET(udp_sd, &readfds)) {
            UDPMessage msg;
            struct sockaddr_in sender;
            socklen_t len = sizeof(sender);
            
            // Loop to read ALL available packets
            while (true) {
                int bytes = recvfrom(udp_sd, &msg, sizeof(UDPMessage), MSG_DONTWAIT, (struct sockaddr*)&sender, &len);
                
                if (bytes <= 0) {
                    // If no more data (EAGAIN/EWOULDBLOCK) or error, stop reading
                    if (errno != EAGAIN && errno != EWOULDBLOCK && bytes == -1) {
                        perror("recvfrom error");
                    }
                    break; 
                }

                if (msg.type == MSG_DRAW) {
                    SDL_SetRenderTarget(renderer, canvasTexture);
                    auto setPixel = [&](int px, int py, Pixel c) {
                        SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, c.a);
                        SDL_RenderDrawPoint(renderer, px, py);
                    };
                    if(msg.id_brush >= 0 && msg.id_brush < availableBrushes.size()) {
                        availableBrushes[msg.id_brush]->paint(msg.x, msg.y, msg.color, msg.size, setPixel);
                    }
                    SDL_SetRenderTarget(renderer, NULL);
                }
                else if (msg.type == MSG_LINE) {
                    SDL_SetRenderTarget(renderer, canvasTexture);
                    // For lines, we need to interpolate with the brush
                    // Simple Bresenham for now, but using brush at each step
                    int x0 = msg.x, y0 = msg.y;
                    int x1 = msg.ex, y1 = msg.ey;
                    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
                    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
                    int err = dx + dy, e2;
                    
                    auto setPixel = [&](int px, int py, Pixel c) {
                        SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, c.a);
                        SDL_RenderDrawPoint(renderer, px, py);
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
                    SDL_SetRenderTarget(renderer, NULL);
                }
                else if (msg.type == MSG_CURSOR) {
                    char key[64];
                    snprintf(key, sizeof(key), "%s:%d", inet_ntoa(sender.sin_addr), ntohs(sender.sin_port));
                    remote_cursors[key] = {msg.x, msg.y, msg.color};
                }

            }
        }
    }
}

void SetupUI(){
    ColorPicker* colp = new ColorPicker();
    colp->x = 10; colp->y = 10; colp->w = 100; colp->h = 100;
    colp->color = {255, 0, 0, 255}; // Base color red
    buttons.push_back(colp);

    HuePicker* hp = new HuePicker();
    hp->x = 10; hp->y = 120; hp->w = 100; hp->h = 20;
    hp->linkedPicker = colp;
    buttons.push_back(hp);

    BrushButton* b1 = new BrushButton();
    b1->x = 120; b1->y = 10; b1->w = 40; b1->h = 40;
    b1->brushId = 0; // Round
    buttons.push_back(b1);

    BrushButton* b2 = new BrushButton();
    b2->x = 120; b2->y = 60; b2->w = 40; b2->h = 40;
    b2->brushId = 1; // Square
    buttons.push_back(b2);

    SizeUpButton* sizeUp = new SizeUpButton();
    sizeUp->x = 170; sizeUp->y = 10; sizeUp->w = 30; sizeUp->h = 30;
    buttons.push_back(sizeUp);

    SizeDownButton* sizeDown = new SizeDownButton();
    sizeDown->x = 170; sizeDown->y = 50; sizeDown->w = 30; sizeDown->h = 30;
    buttons.push_back(sizeDown);
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("Usage: %s <ip> <port>\n", argv[0]);
        return 1;
    }

    setup_network(argv[1], atoi(argv[2]));
    send_tcp_login("User1"); // Example login

    SDL_Window* window = NULL;
    SDL_Renderer* renderer = NULL;
    if (!init_sdl(&window, &renderer)) {
        return 1;
    }

    receive_initial_canvas(renderer);

    // Initialize Brushes
    availableBrushes.push_back(new RoundBrush());
    availableBrushes.push_back(new SquareBrush());
    availableBrushes[1]->size = 10; // Make square brush larger by default

    SetupUI();

    draw_ui(renderer);

    bool quit = false;
    while (!quit) {
        handle_input(renderer, &quit);
        handle_network(renderer, &quit);
        draw_ui(renderer);
        SDL_Delay(10);
    }

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    close(tcp_sd);
    close(udp_sd);
    return 0;
}
