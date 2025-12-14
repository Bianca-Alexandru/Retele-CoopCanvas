/*
   UI Header - Shared Canvas Client
   
   Contains all UI components:
   - Button base class and derived button types
   - Layer buttons on right side
   - SetupUI() - Creates all buttons
   - draw_ui() - Renders the UI
*/

#ifndef UI_H
#define UI_H

#include <SDL2/SDL.h>
#include <vector>
#include <map>
#include <string>
#include <cmath>
#include <functional>
#include "brushes.h"

using namespace std;

/*****************************************************************************
   EXTERNAL REFERENCES (defined in client.cpp)
 *****************************************************************************/

extern SDL_Color userColor;
extern int currentBrushId;
extern int currentCanvasId;
extern int currentLayerId;
extern int layerCount;
extern int layerDisplayIds[];
extern int loggedin;
extern bool isEyedropping;
extern vector<Brush*> availableBrushes;
extern SDL_Texture* canvasTexture;
extern SDL_Texture* layerTextures[];
extern uint8_t layerOpacity[];

struct RemoteCursor {
    int x, y;
    SDL_Color color;
};
extern map<string, RemoteCursor> remote_cursors;

extern void send_tcp_login(const char* username);
extern void send_tcp_save();
extern void send_tcp_add_layer(int layer_id = 0);
extern void send_tcp_delete_layer(int layer_id);
extern void UpdateLayerButtons();
extern void perform_undo();
extern void perform_redo();
extern void save_undo_state();
extern void download_as_bmp();
extern vector<CanvasSnapshot*> redoStack;

extern SDL_Texture* signatureTexture;
extern SDL_Rect signatureRect;
extern bool isDrawingSignature;
extern void clear_signature(SDL_Renderer* renderer);

#define UI_WIDTH 640
#define UI_HEIGHT 480

/*****************************************************************************
   BUTTON BASE CLASS
 *****************************************************************************/

class Button {
public:
    int x, y, w, h;
    SDL_Color color;
    virtual void Draw(SDL_Renderer* renderer) = 0;
    virtual void Click() = 0;
    virtual ~Button() {}
};

extern vector<Button*> buttons;

inline void draw_digit(SDL_Renderer* renderer, int digit, int x, int y, int size) {
    int w = size, h = size * 2;
    bool segs[10][7] = {
        {1,1,1,1,1,1,0}, {0,1,1,0,0,0,0}, {1,1,0,1,1,0,1}, {1,1,1,1,0,0,1}, {0,1,1,0,0,1,1},
        {1,0,1,1,0,1,1}, {1,0,1,1,1,1,1}, {1,1,1,0,0,0,0}, {1,1,1,1,1,1,1}, {1,1,1,1,0,1,1}
    };
    if (digit < 0 || digit > 9) return;
    if (segs[digit][0]) SDL_RenderDrawLine(renderer, x, y, x+w, y);
    if (segs[digit][1]) SDL_RenderDrawLine(renderer, x+w, y, x+w, y+h/2);
    if (segs[digit][2]) SDL_RenderDrawLine(renderer, x+w, y+h/2, x+w, y+h);
    if (segs[digit][3]) SDL_RenderDrawLine(renderer, x, y+h, x+w, y+h);
    if (segs[digit][4]) SDL_RenderDrawLine(renderer, x, y+h/2, x, y+h);
    if (segs[digit][5]) SDL_RenderDrawLine(renderer, x, y, x, y+h/2);
    if (segs[digit][6]) SDL_RenderDrawLine(renderer, x, y+h/2, x+w, y+h/2);
}

/*****************************************************************************
   COLOR PICKER BUTTONS
 *****************************************************************************/

class ColorPicker : public Button {
public:
    virtual void Draw(SDL_Renderer* renderer) override {
        for (int i = 0; i < w; i++) {
            for (int j = 0; j < h; j++) {
                float sat = (float)i / w;
                int r_s = 255 + (color.r - 255) * sat;
                int g_s = 255 + (color.g - 255) * sat;
                int b_s = 255 + (color.b - 255) * sat;
                float val = 1.0f - (float)j / h;
                SDL_SetRenderDrawColor(renderer, (Uint8)(r_s * val), (Uint8)(g_s * val), (Uint8)(b_s * val), 255);
                SDL_RenderDrawPoint(renderer, x + i, y + j);
            }
        }
        
        // Draw marker for current color
        // Removed old marker code to avoid duplicate previews

        // Draw Large Preview Box (Right of Color Picker, below Eyedropper)
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_Rect previewBorder = {x + w + 10, y + 45, 30, 30};
        SDL_RenderDrawRect(renderer, &previewBorder);
        SDL_SetRenderDrawColor(renderer, userColor.r, userColor.g, userColor.b, 255);
        SDL_Rect previewFill = {x + w + 11, y + 46, 28, 28};
        SDL_RenderFillRect(renderer, &previewFill);
    }

    virtual void Click() override {
        int mx, my;
        SDL_GetMouseState(&mx, &my);
        int rel_x = mx - x, rel_y = my - y;
        if (rel_x < 0) rel_x = 0; if (rel_x >= w) rel_x = w - 1;
        if (rel_y < 0) rel_y = 0; if (rel_y >= h) rel_y = h - 1;

        float sat = (float)rel_x / w;
        int r_s = 255 + (color.r - 255) * sat;
        int g_s = 255 + (color.g - 255) * sat;
        int b_s = 255 + (color.b - 255) * sat;
        float val = 1.0f - (float)rel_y / h;
        
        userColor.r = (Uint8)(r_s * val);
        userColor.g = (Uint8)(g_s * val);
        userColor.b = (Uint8)(b_s * val);
        userColor.a = 255;
        ::printf("[Client][UI] Color changed to RGBA(%d,%d,%d,%d)\n", userColor.r, userColor.g, userColor.b, userColor.a);
    }
};

class HuePicker : public Button {
public:
    ColorPicker* linkedPicker;

    virtual void Draw(SDL_Renderer* renderer) override {
        for (int i = 0; i < w; i++) {
            float hue = (float)i / w * 6.0f;
            float x_val = 1 - fabs(fmod(hue, 2) - 1);
            int r = 0, g = 0, b = 0;
            if (hue < 1) { r = 255; g = x_val * 255; b = 0; }
            else if (hue < 2) { r = x_val * 255; g = 255; b = 0; }
            else if (hue < 3) { r = 0; g = 255; b = x_val * 255; }
            else if (hue < 4) { r = 0; g = x_val * 255; b = 255; }
            else if (hue < 5) { r = x_val * 255; g = 0; b = 255; }
            else { r = 255; g = 0; b = x_val * 255; }
            SDL_SetRenderDrawColor(renderer, r, g, b, 255);
            SDL_RenderDrawLine(renderer, x + i, y, x + i, y + h);
        }
    }

    virtual void Click() override {
        int mx, my;
        SDL_GetMouseState(&mx, &my);
        int rel_x = mx - x;
        if (rel_x < 0) rel_x = 0; if (rel_x >= w) rel_x = w - 1;

        float hue = (float)rel_x / w * 6.0f;
        float x_val = 1 - fabs(fmod(hue, 2) - 1);
        int r = 0, g = 0, b = 0;
        if (hue < 1) { r = 255; g = x_val * 255; b = 0; }
        else if (hue < 2) { r = x_val * 255; g = 255; b = 0; }
        else if (hue < 3) { r = 0; g = 255; b = x_val * 255; }
        else if (hue < 4) { r = 0; g = x_val * 255; b = 255; }
        else if (hue < 5) { r = x_val * 255; g = 0; b = 255; }
        else { r = 255; g = 0; b = x_val * 255; }

        if (linkedPicker) {
            linkedPicker->color = {(Uint8)r, (Uint8)g, (Uint8)b, 255};
            // Auto-update userColor to the pure hue when hue changes
            userColor = linkedPicker->color;
            ::printf("[Client][UI] Hue changed, base color now RGB(%d,%d,%d)\n", r, g, b);
        }
    }
};

/*****************************************************************************
   BRUSH BUTTONS
 *****************************************************************************/

class BrushButton : public Button {
public:
    int brushId;
    
    virtual void Draw(SDL_Renderer* renderer) override {
        if (currentBrushId == brushId) {
            SDL_SetRenderDrawColor(renderer, 150, 150, 150, 255);
        } else {
            SDL_SetRenderDrawColor(renderer, 220, 220, 220, 255);
        }
        SDL_Rect rect = {x, y, w, h};
        SDL_RenderFillRect(renderer, &rect);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderDrawRect(renderer, &rect);

        if (brushId == 0) {
            for (int i = -3; i <= 3; i++)
                for (int j = -3; j <= 3; j++)
                    if (i*i + j*j <= 9) SDL_RenderDrawPoint(renderer, x+w/2+i, y+h/2+j);
        } else if (brushId == 1) {
            SDL_Rect r = {x+w/2-3, y+h/2-3, 7, 7};
            SDL_RenderFillRect(renderer, &r);
        } else if (brushId == 2) {
            SDL_SetRenderDrawColor(renderer, 255, 200, 200, 255);
            SDL_Rect r = {x+w/2-4, y+h/2-4, 9, 9};
            SDL_RenderFillRect(renderer, &r);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderDrawRect(renderer, &r);
        } else if (brushId == 3) {
            // Pressure brush icon (P)
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            int cx = x+w/2, cy = y+h/2;
            SDL_RenderDrawLine(renderer, cx-2, cy-4, cx-2, cy+4); // Vertical
            SDL_RenderDrawLine(renderer, cx-2, cy-4, cx+2, cy-4); // Top
            SDL_RenderDrawLine(renderer, cx+2, cy-4, cx+2, cy);   // Right
            SDL_RenderDrawLine(renderer, cx-2, cy, cx+2, cy);     // Middle
        } else if (brushId == 4) {
            // Airbrush icon (A)
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            int cx = x+w/2, cy = y+h/2;
            // Draw 'A'
            SDL_RenderDrawLine(renderer, cx, cy-5, cx-3, cy+5);
            SDL_RenderDrawLine(renderer, cx, cy-5, cx+3, cy+5);
            SDL_RenderDrawLine(renderer, cx-2, cy, cx+2, cy);
        }
    }
    
    virtual void Click() override {
        currentBrushId = brushId;
        ::printf("[Client][UI] Brush switched to %d\n", brushId);
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
        int cx = x+w/2, cy = y+h/2;
        SDL_RenderDrawLine(renderer, cx-5, cy, cx+5, cy);
        SDL_RenderDrawLine(renderer, cx, cy-5, cx, cy+5);
    }
    
    virtual void Click() override {
        if (currentBrushId >= 0 && currentBrushId < (int)availableBrushes.size()) {
            availableBrushes[currentBrushId]->size++;
            ::printf("[Client][UI] Brush size increased to %d\n", availableBrushes[currentBrushId]->size);
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
        int cx = x+w/2, cy = y+h/2;
        SDL_RenderDrawLine(renderer, cx-5, cy, cx+5, cy);
    }
    
    virtual void Click() override {
        if (currentBrushId >= 0 && currentBrushId < (int)availableBrushes.size()) {
            if (availableBrushes[currentBrushId]->size > 1) {
                availableBrushes[currentBrushId]->size--;
                ::printf("[Client][UI] Brush size decreased to %d\n", availableBrushes[currentBrushId]->size);
            }
        }
    }
};

/*****************************************************************************
   LOGIN & LOBBY BUTTONS
 *****************************************************************************/

class LoginButton : public Button {
public:
    virtual void Draw(SDL_Renderer* renderer) override {
        SDL_SetRenderDrawColor(renderer, 70, 130, 180, 255);
        SDL_Rect rect = {x, y, w, h};
        SDL_RenderFillRect(renderer, &rect);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        SDL_RenderDrawRect(renderer, &rect);
        int cx = x+w/2, cy = y+h/2;
        SDL_RenderDrawLine(renderer, cx-20, cy-8, cx-20, cy+8);
        SDL_RenderDrawLine(renderer, cx-20, cy+8, cx-10, cy+8);
        SDL_RenderDrawLine(renderer, cx-5, cy-8, cx+5, cy-8);
        SDL_RenderDrawLine(renderer, cx-5, cy+8, cx+5, cy+8);
        SDL_RenderDrawLine(renderer, cx-5, cy-8, cx-5, cy+8);
        SDL_RenderDrawLine(renderer, cx+5, cy-8, cx+5, cy+8);
        SDL_RenderDrawLine(renderer, cx+10, cy-8, cx+20, cy-8);
        SDL_RenderDrawLine(renderer, cx+10, cy-8, cx+10, cy+8);
        SDL_RenderDrawLine(renderer, cx+10, cy+8, cx+20, cy+8);
        SDL_RenderDrawLine(renderer, cx+15, cy, cx+20, cy);
        SDL_RenderDrawLine(renderer, cx+20, cy, cx+20, cy+8);
    }
    
    virtual void Click() override {
        ::printf("[Client][UI] Login button clicked for canvas #%d\n", currentCanvasId);
        send_tcp_login("username");  // Hardcoded username for now
    }
};

class LobbyLeftButton : public Button {
public:
    virtual void Draw(SDL_Renderer* renderer) override {
        SDL_SetRenderDrawColor(renderer, 100, 100, 150, 255);
        SDL_Rect rect = {x, y, w, h};
        SDL_RenderFillRect(renderer, &rect);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        SDL_RenderDrawRect(renderer, &rect);
        int cx = x+w/2, cy = y+h/2;
        SDL_RenderDrawLine(renderer, cx+5, cy-10, cx-5, cy);
        SDL_RenderDrawLine(renderer, cx-5, cy, cx+5, cy+10);
    }
    
    virtual void Click() override {
        if (currentCanvasId > 0) {
            currentCanvasId--;
            ::printf("[Client][UI] Lobby changed to canvas #%d\n", currentCanvasId);
        }
    }
};

class LobbyRightButton : public Button {
public:
    virtual void Draw(SDL_Renderer* renderer) override {
        SDL_SetRenderDrawColor(renderer, 100, 100, 150, 255);
        SDL_Rect rect = {x, y, w, h};
        SDL_RenderFillRect(renderer, &rect);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        SDL_RenderDrawRect(renderer, &rect);
        int cx = x+w/2, cy = y+h/2;
        SDL_RenderDrawLine(renderer, cx-5, cy-10, cx+5, cy);
        SDL_RenderDrawLine(renderer, cx+5, cy, cx-5, cy+10);
    }
    
    virtual void Click() override {
        if (currentCanvasId < 9) {
            currentCanvasId++;
            ::printf("[Client][UI] Lobby changed to canvas #%d\n", currentCanvasId);
        }
    }
};

/*****************************************************************************
   SAVE BUTTON
 *****************************************************************************/

class SaveButton : public Button {
public:
    virtual void Draw(SDL_Renderer* renderer) override {
        SDL_SetRenderDrawColor(renderer, 50, 150, 50, 255);
        SDL_Rect rect = {x, y, w, h};
        SDL_RenderFillRect(renderer, &rect);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        SDL_Rect inner = {x+8, y+5, w-16, h-15};
        SDL_RenderDrawRect(renderer, &inner);
        SDL_Rect slot = {x+12, y+5, w-24, 6};
        SDL_RenderFillRect(renderer, &slot);
    }
    
    virtual void Click() override {
        ::printf("[Client][UI] Save button clicked\n");
        send_tcp_save();
    }
};

/*****************************************************************************
   LAYER BUTTONS (Right side of screen)
 *****************************************************************************/

extern void send_tcp_reorder_layer(int old_idx, int new_idx);

// Drag state
int dragLayerId = -1;
int dragStartY = -1;
int dragCurrentY = -1;

class LayerButton : public Button {
public:
    int layerId;
    
    virtual void Draw(SDL_Renderer* renderer) override {
        // If dragging this button, draw at drag position
        int drawY = y;
        if (dragLayerId == layerId) {
            drawY = dragCurrentY - h/2;
        }
        
        if (currentLayerId == layerId) {
            SDL_SetRenderDrawColor(renderer, 100, 200, 100, 255);
        } else {
            SDL_SetRenderDrawColor(renderer, 150, 150, 150, 255);
        }
        
        SDL_Rect rect = {x, drawY, w, h};
        SDL_RenderFillRect(renderer, &rect);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderDrawRect(renderer, &rect);
        
        // Draw layer number
        draw_digit(renderer, layerDisplayIds[layerId], x + w/2, drawY + h/2, 10);
    }
    
    virtual void Click() override {
        // Click logic handled in MouseDown/Up now for drag
        currentLayerId = layerId;
    }
};

class AddLayerButton : public Button {
public:
    virtual void Draw(SDL_Renderer* renderer) override {
        SDL_SetRenderDrawColor(renderer, 100, 200, 100, 255);  // Green
        SDL_Rect rect = {x, y, w, h};
        SDL_RenderFillRect(renderer, &rect);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        int cx = x+w/2, cy = y+h/2;
        SDL_RenderDrawLine(renderer, cx-4, cy, cx+4, cy);
        SDL_RenderDrawLine(renderer, cx, cy-4, cx, cy+4);
    }
    
    virtual void Click() override {
        ::printf("[Client][UI] Add layer button clicked\n");
        save_undo_state(); // Save state before adding
        send_tcp_add_layer();
        // layerCount will be updated when server broadcasts MSG_LAYER_ADD
    }
};

class DeleteLayerButton : public Button {
public:
    virtual void Draw(SDL_Renderer* renderer) override {
        SDL_SetRenderDrawColor(renderer, 200, 100, 100, 255);  // Red
        SDL_Rect rect = {x, y, w, h};
        SDL_RenderFillRect(renderer, &rect);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        int cx = x+w/2, cy = y+h/2;
        SDL_RenderDrawLine(renderer, cx-4, cy, cx+4, cy);
    }
    
    virtual void Click() override {
        if (layerCount > 2 && currentLayerId > 0) {
            ::printf("[Client][UI] Delete layer button clicked for layer %d\n", currentLayerId);
            save_undo_state(); // Save state before deleting
            send_tcp_delete_layer(currentLayerId);
            // layerCount and currentLayerId will be updated when server broadcasts MSG_LAYER_DEL
        } else {
            ::printf("[Client][UI] Cannot delete layer: must keep at least 1 drawable layer\n");
        }
    }
};

/*****************************************************************************
   UNDO/REDO BUTTONS (Bottom of screen)
 *****************************************************************************/

class UndoButton : public Button {
public:
    virtual void Draw(SDL_Renderer* renderer) override {
        SDL_SetRenderDrawColor(renderer, 80, 80, 120, 255);  // Blue-grey
        SDL_Rect rect = {x, y, w, h};
        SDL_RenderFillRect(renderer, &rect);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderDrawRect(renderer, &rect);
        
        // Draw curved arrow pointing left (undo symbol)
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        int cx = x + w/2, cy = y + h/2;
        // Arrow shaft
        SDL_RenderDrawLine(renderer, cx + 8, cy, cx - 4, cy);
        // Arrow head
        SDL_RenderDrawLine(renderer, cx - 4, cy, cx + 2, cy - 5);
        SDL_RenderDrawLine(renderer, cx - 4, cy, cx + 2, cy + 5);
    }
    
    virtual void Click() override {
        perform_undo();
    }
};

class RedoButton : public Button {
public:
    virtual void Draw(SDL_Renderer* renderer) override {
        if (redoStack.empty()) return; // Don't draw if nothing to redo

        SDL_SetRenderDrawColor(renderer, 80, 80, 120, 255);  // Blue-grey
        SDL_Rect rect = {x, y, w, h};
        SDL_RenderFillRect(renderer, &rect);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderDrawRect(renderer, &rect);
        
        // Draw curved arrow pointing right (redo symbol)
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        int cx = x + w/2, cy = y + h/2;
        // Arrow shaft
        SDL_RenderDrawLine(renderer, cx - 8, cy, cx + 4, cy);
        // Arrow head
        SDL_RenderDrawLine(renderer, cx + 4, cy, cx - 2, cy - 5);
        SDL_RenderDrawLine(renderer, cx + 4, cy, cx - 2, cy + 5);
    }
    
    virtual void Click() override {
        perform_redo();
    }
};

class DownloadButton : public Button {
public:
    virtual void Draw(SDL_Renderer* renderer) override {
        SDL_SetRenderDrawColor(renderer, 150, 75, 0, 255);  // Brown
        SDL_Rect rect = {x, y, w, h};
        SDL_RenderFillRect(renderer, &rect);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        SDL_Rect inner = {x+10, y+5, w-20, h-15};
        SDL_RenderDrawRect(renderer, &inner);
        // Draw downward arrow
        int cx = x + w/2, cy = y + h/2;
        SDL_RenderDrawLine(renderer, cx, cy - 5, cx, cy + 5);
        SDL_RenderDrawLine(renderer, cx - 5, cy + 2, cx, cy + 5);
        SDL_RenderDrawLine(renderer, cx + 5, cy + 2, cx, cy + 5);
    }

    virtual void Click() override {
        download_as_bmp();
        ::printf("[Client][UI] Download button clicked\n");
    }
};

/*****************************************************************************
   HELPER FUNCTIONS
 *****************************************************************************/

class EyedropperButton : public Button {
public:
    virtual void Draw(SDL_Renderer* renderer) override {
        if (isEyedropping) {
            SDL_SetRenderDrawColor(renderer, 100, 100, 255, 255); // Blue when active
        } else {
            SDL_SetRenderDrawColor(renderer, 200, 200, 200, 255);
        }
        SDL_Rect rect = {x, y, w, h};
        SDL_RenderFillRect(renderer, &rect);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderDrawRect(renderer, &rect);
        
        // Draw icon (simple pipette)
        int cx = x + w/2, cy = y + h/2;
        SDL_RenderDrawLine(renderer, cx-3, cy-3, cx+3, cy+3);
        SDL_RenderDrawLine(renderer, cx-3, cy-3, cx, cy-6);
        SDL_RenderDrawLine(renderer, cx+3, cy+3, cx+6, cy);
        SDL_RenderDrawLine(renderer, cx, cy-6, cx+6, cy);
    }
    
    virtual void Click() override {
        isEyedropping = !isEyedropping;
        ::printf("[Client][UI] Eyedropper toggled: %s\n", isEyedropping ? "ON" : "OFF");
    }
};

// Button indices
#define SAVE_BTN_IDX 13
#define ADD_LAYER_BTN_IDX 14
#define DEL_LAYER_BTN_IDX 15
#define UNDO_BTN_IDX 16
#define REDO_BTN_IDX 17
#define EYEDROPPER_BTN_IDX 18
#define LAYER_BUTTONS_START 19

inline void SetupUI() {
    ::printf("[Client][UI] Setting up UI buttons...\n");
    
    // === LOGIN SCREEN BUTTONS (indices 0, 1, 2) ===
    LoginButton* loginBtn = new LoginButton();
    loginBtn->x = UI_WIDTH/2 - 75; loginBtn->y = 300; loginBtn->w = 150; loginBtn->h = 50; // Moved to bottom
    buttons.push_back(loginBtn);

    LobbyLeftButton* lobbyLeft = new LobbyLeftButton();
    lobbyLeft->x = UI_WIDTH/2 - 120; lobbyLeft->y = 50; lobbyLeft->w = 40; lobbyLeft->h = 50; // Moved to top
    buttons.push_back(lobbyLeft);

    LobbyRightButton* lobbyRight = new LobbyRightButton();
    lobbyRight->x = UI_WIDTH/2 + 80; lobbyRight->y = 50; lobbyRight->w = 40; lobbyRight->h = 50; // Moved to top
    buttons.push_back(lobbyRight);

    // === CANVAS UI BUTTONS (indices 3-9) ===
    ColorPicker* colp = new ColorPicker();
    colp->x = 10; colp->y = 10; colp->w = 100; colp->h = 100;
    colp->color = {255, 0, 0, 255};
    buttons.push_back(colp);

    HuePicker* hp = new HuePicker();
    hp->x = 10; hp->y = 120; hp->w = 100; hp->h = 20;
    hp->linkedPicker = colp;
    buttons.push_back(hp);

    // Size/Download buttons (Horizontal below HuePicker)
    SizeUpButton* sizeUp = new SizeUpButton(); sizeUp->x = 10; sizeUp->y = 150; sizeUp->w = 30; sizeUp->h = 30;
    buttons.push_back(sizeUp);
    SizeDownButton* sizeDown = new SizeDownButton(); sizeDown->x = 45; sizeDown->y = 150; sizeDown->w = 30; sizeDown->h = 30;
    buttons.push_back(sizeDown);
    DownloadButton* downloadBtn = new DownloadButton();
    downloadBtn->x = 80; downloadBtn->y = 150; downloadBtn->w = 30; downloadBtn->h = 30;
    buttons.push_back(downloadBtn);

    // Brushes (Vertical on left side, below Size buttons)
    BrushButton* b1 = new BrushButton(); b1->x = 10; b1->y = 190; b1->w = 30; b1->h = 30; b1->brushId = 0;
    buttons.push_back(b1);
    BrushButton* b2 = new BrushButton(); b2->x = 10; b2->y = 225; b2->w = 30; b2->h = 30; b2->brushId = 1;
    buttons.push_back(b2);
    BrushButton* b3 = new BrushButton(); b3->x = 10; b3->y = 260; b3->w = 30; b3->h = 30; b3->brushId = 2;
    buttons.push_back(b3);
    // Pressure Brush (ID 3)
    BrushButton* b4 = new BrushButton(); b4->x = 10; b4->y = 295; b4->w = 30; b4->h = 30; b4->brushId = 3;
    buttons.push_back(b4);
    // Airbrush (ID 4)
    BrushButton* b5 = new BrushButton(); b5->x = 10; b5->y = 330; b5->w = 30; b5->h = 30; b5->brushId = 4;
    buttons.push_back(b5);

    // === SAVE BUTTON (index 11) - Top center ===
    SaveButton* saveBtn = new SaveButton();
    saveBtn->x = UI_WIDTH/2 - 25; saveBtn->y = 10; saveBtn->w = 50; saveBtn->h = 30;
    buttons.push_back(saveBtn);

    // === LAYER CONTROL BUTTONS (indices 11, 12) ===
    // Add layer button (green, top-left of layer panel)
    AddLayerButton* addLayer = new AddLayerButton();
    addLayer->x = UI_WIDTH - 50; addLayer->y = 10; addLayer->w = 20; addLayer->h = 20;
    buttons.push_back(addLayer);

    // Delete layer button (red, top-right of layer panel)
    DeleteLayerButton* delLayer = new DeleteLayerButton();
    delLayer->x = UI_WIDTH - 25; delLayer->y = 10; delLayer->w = 20; delLayer->h = 20;
    buttons.push_back(delLayer);

    // === UNDO/REDO BUTTONS (indices 13, 14) - Bottom of screen ===
    UndoButton* undoBtn = new UndoButton();
    undoBtn->x = UI_WIDTH/2 - 70; undoBtn->y = UI_HEIGHT - 40; undoBtn->w = 60; undoBtn->h = 30;
    buttons.push_back(undoBtn);

    RedoButton* redoBtn = new RedoButton();
    redoBtn->x = UI_WIDTH/2 + 10; redoBtn->y = UI_HEIGHT - 40; redoBtn->w = 60; redoBtn->h = 30;
    buttons.push_back(redoBtn);

    // === EYEDROPPER BUTTON (index 16) - Right of colors ===
    EyedropperButton* eyeBtn = new EyedropperButton();
    eyeBtn->x = 120; eyeBtn->y = 10; eyeBtn->w = 30; eyeBtn->h = 30;
    eyeBtn->color = {220, 220, 220, 255}; // Explicit color
    buttons.push_back(eyeBtn);

    // === LAYER BUTTONS (indices 17+) - Will be created dynamically ===
    // Initial layer buttons for layers 1-2
    UpdateLayerButtons();

    ::printf("[Client][UI] UI setup complete: %zu buttons\n", buttons.size());
}

inline void UpdateLayerButtons() {
    // Remove old layer buttons (keep indices 0-15)
    while (buttons.size() > LAYER_BUTTONS_START) {
        delete buttons.back();
        buttons.pop_back();
    }
    
    // Add layer buttons in DESCENDING order (highest layer at top)
    // Layer count includes layer 0 (paper), so drawable layers are 1 to layerCount-1
    int numDrawableLayers = layerCount - 1;
    for (int i = numDrawableLayers; i >= 1; i--) {
        LayerButton* lb = new LayerButton();
        lb->x = UI_WIDTH - 45; 
        lb->y = 40 + (numDrawableLayers - i) * 35;  // Position from top
        lb->w = 35; 
        lb->h = 30;
        lb->layerId = i;
        buttons.push_back(lb);
    }
}

inline void draw_ui(SDL_Renderer* renderer, std::function<void(SDL_Renderer*)> postCanvasCallback = nullptr) {
    SDL_SetRenderDrawColor(renderer, 200, 200, 200, 255);
    SDL_RenderClear(renderer);

    if (!loggedin) {
        SDL_SetRenderDrawColor(renderer, 50, 50, 80, 255);
        SDL_Rect bg = {0, 0, UI_WIDTH, UI_HEIGHT};
        SDL_RenderFillRect(renderer, &bg);
        
        // Draw Signature Box
        signatureRect.x = UI_WIDTH/2 - signatureRect.w/2;
        signatureRect.y = 150; // Middle
        
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        SDL_RenderFillRect(renderer, &signatureRect);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderDrawRect(renderer, &signatureRect);
        
        // Render the signature texture onto the screen
        if (signatureTexture) {
            SDL_RenderCopy(renderer, signatureTexture, NULL, &signatureRect);
        }
        
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        draw_digit(renderer, currentCanvasId, UI_WIDTH/2 - 10, 55, 20); // Top between arrows
        // SDL_RenderDrawLine(renderer, UI_WIDTH/2 - 40, UI_HEIGHT/2 + 75, UI_WIDTH/2 + 40, UI_HEIGHT/2 + 75);
        
        if (buttons.size() > 0) buttons[0]->Draw(renderer);
        if (buttons.size() > 1) buttons[1]->Draw(renderer);
        if (buttons.size() > 2) buttons[2]->Draw(renderer);
        
        SDL_RenderPresent(renderer);
        return;
    }

    // Render all layers (GPU Compositing)
    // Layer 0 is background (paper), drawn first.
    for (int i = 0; i < layerCount; i++) {
        if (layerTextures[i]) {
            // Apply opacity
            SDL_SetTextureAlphaMod(layerTextures[i], layerOpacity[i]);
            SDL_RenderCopy(renderer, layerTextures[i], NULL, NULL);
        }
    }
    // SDL_RenderCopy(renderer, canvasTexture, NULL, NULL); // Deprecated

    if (postCanvasCallback) {
        postCanvasCallback(renderer);
    }

    // Draw all canvas UI buttons (tools, save, layer controls, undo/redo, layers)
    for (size_t i = 3; i < buttons.size(); i++) {
        buttons[i]->Draw(renderer);
    }

    // Draw remote cursors
    for (auto const& [key, cursor] : remote_cursors) {
        SDL_SetRenderDrawColor(renderer, cursor.color.r, cursor.color.g, cursor.color.b, 255);
        int size = 5, gap = 2;
        SDL_RenderDrawLine(renderer, cursor.x - size, cursor.y, cursor.x - gap, cursor.y);
        SDL_RenderDrawLine(renderer, cursor.x + gap, cursor.y, cursor.x + size, cursor.y);
        SDL_RenderDrawLine(renderer, cursor.x, cursor.y - size, cursor.x, cursor.y - gap);
        SDL_RenderDrawLine(renderer, cursor.x, cursor.y + gap, cursor.x, cursor.y + size);
    }

    // Draw local eyedropper preview if active
    if (isEyedropping) {
        // Preview box removed as requested
    } else {
        // Draw brush cursor preview (circle outline)
        int mx, my;
        SDL_GetMouseState(&mx, &my);
        
        // Only draw if over canvas and not over UI buttons (roughly)
        // Simple check: if x > 150 (right of tools) or y > 100 (below color picker)
        // But tools are on left...
        // Let's just draw it if it's within canvas bounds
        if (mx >= 0 && mx < CANVAS_WIDTH && my >= 0 && my < CANVAS_HEIGHT) {
            if (currentBrushId >= 0 && currentBrushId < (int)availableBrushes.size()) {
                int size = availableBrushes[currentBrushId]->size;
                SDL_SetRenderDrawColor(renderer, 100, 100, 100, 128); // Grey outline
                
                // Simple circle drawing algorithm
                int r = size;
                int cx = mx;
                int cy = my;
                
                // Midpoint circle algorithm
                int x = r;
                int y = 0;
                int err = 0;
                
                while (x >= y) {
                    SDL_RenderDrawPoint(renderer, cx + x, cy + y);
                    SDL_RenderDrawPoint(renderer, cx + y, cy + x);
                    SDL_RenderDrawPoint(renderer, cx - y, cy + x);
                    SDL_RenderDrawPoint(renderer, cx - x, cy + y);
                    SDL_RenderDrawPoint(renderer, cx - x, cy - y);
                    SDL_RenderDrawPoint(renderer, cx - y, cy - x);
                    SDL_RenderDrawPoint(renderer, cx + y, cy - x);
                    SDL_RenderDrawPoint(renderer, cx + x, cy - y);
                    
                    if (err <= 0) {
                        y += 1;
                        err += 2*y + 1;
                    }
                    if (err > 0) {
                        x -= 1;
                        err -= 2*x + 1;
                    }
                }
            }
        }
    }

    // Draw remote signature if available
    // if (remoteSignatureTexture) {
    //     SDL_RenderCopy(renderer, remoteSignatureTexture, NULL, &remoteSignatureRect);
    // }

    SDL_RenderPresent(renderer);
}

inline bool handle_login_screen_click(int x, int y) {
    // Check signature box
    if (x >= signatureRect.x && x < signatureRect.x + signatureRect.w &&
        y >= signatureRect.y && y < signatureRect.y + signatureRect.h) {
        isDrawingSignature = true;
        return true;
    }

    for (int i = 0; i <= 2 && i < (int)buttons.size(); i++) {
        Button* btn = buttons[i];
        if (x >= btn->x && x < btn->x + btn->w && y >= btn->y && y < btn->y + btn->h) {
            btn->Click();
            return true;
        }
    }
    return false;
}

// Check if point is inside button
inline bool point_in_button(Button* btn, int x, int y) {
    return x >= btn->x && x < btn->x + btn->w && y >= btn->y && y < btn->y + btn->h;
}

inline bool handle_canvas_ui_click(int x, int y) {
    // Iterate through all canvas UI buttons (indices 3+)
    for (size_t i = 3; i < buttons.size(); i++) {
        Button* btn = buttons[i];
        // Special check for Redo button visibility
        if (i == REDO_BTN_IDX && redoStack.empty()) continue;

        if (point_in_button(btn, x, y)) {
            // Check if it's a layer button for drag start
            if (i >= LAYER_BUTTONS_START) {
                LayerButton* lb = (LayerButton*)btn;
                dragLayerId = lb->layerId;
                dragStartY = y;
                dragCurrentY = y;
                ::printf("[Client][UI] Started dragging layer %d\n", dragLayerId);
            }
            
            btn->Click();
            // Layer add/delete buttons need UI refresh
            if (i == ADD_LAYER_BTN_IDX || i == DEL_LAYER_BTN_IDX) {
                UpdateLayerButtons();
            }
            return true;
        }
    }
    return false;
}

inline void handle_drag_end(int x, int y) {
    if (dragLayerId != -1) {
        // Calculate new index based on drop position
        // Layer buttons are at y = 40 + (numDrawableLayers - i) * 35
        // Inverse: i = numDrawableLayers - (y - 40) / 35
        int numDrawableLayers = layerCount - 1;
        int newIdx = numDrawableLayers - (y - 40) / 35;
        
        // Clamp
        if (newIdx < 1) newIdx = 1;
        if (newIdx > numDrawableLayers) newIdx = numDrawableLayers;
        
        if (newIdx != dragLayerId) {
            ::printf("[Client][UI] Dropped layer %d at index %d\n", dragLayerId, newIdx);
            send_tcp_reorder_layer(dragLayerId, newIdx);
        }
        
        dragLayerId = -1;
    }
}

#endif // UI_H
