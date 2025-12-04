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
#include "brushes.h"

using namespace std;

/* ============================================================================
   EXTERNAL REFERENCES (defined in client.cpp)
   ============================================================================ */

extern SDL_Color userColor;
extern int currentBrushId;
extern int currentCanvasId;
extern int currentLayerId;
extern int layerCount;
extern int loggedin;
extern vector<Brush*> availableBrushes;
extern SDL_Texture* canvasTexture;

struct RemoteCursor {
    int x, y;
    SDL_Color color;
};
extern map<string, RemoteCursor> remote_cursors;

extern void send_tcp_login(const char* username);
extern void send_tcp_save();
extern void send_tcp_add_layer();
extern void send_tcp_delete_layer(int layer_id);
extern void perform_undo();
extern void perform_redo();

#define UI_WIDTH 640
#define UI_HEIGHT 480

/* ============================================================================
   BUTTON BASE CLASS
   ============================================================================ */

class Button {
public:
    int x, y, w, h;
    SDL_Color color;
    virtual void Draw(SDL_Renderer* renderer) = 0;
    virtual void Click() = 0;
    virtual ~Button() {}
};

extern vector<Button*> buttons;

/* ============================================================================
   COLOR PICKER BUTTONS
   ============================================================================ */

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
        printf("[Client][UI] Color changed to RGBA(%d,%d,%d,%d)\n", userColor.r, userColor.g, userColor.b, userColor.a);
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
            printf("[Client][UI] Hue changed, base color now RGB(%d,%d,%d)\n", r, g, b);
        }
    }
};

/* ============================================================================
   BRUSH BUTTONS
   ============================================================================ */

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
        }
    }
    
    virtual void Click() override {
        currentBrushId = brushId;
        printf("[Client][UI] Brush switched to %d\n", brushId);
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
            printf("[Client][UI] Brush size increased to %d\n", availableBrushes[currentBrushId]->size);
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
                printf("[Client][UI] Brush size decreased to %d\n", availableBrushes[currentBrushId]->size);
            }
        }
    }
};

/* ============================================================================
   LOGIN & LOBBY BUTTONS
   ============================================================================ */

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
        printf("[Client][UI] Login button clicked for canvas #%d\n", currentCanvasId);
        send_tcp_login("User1");
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
            printf("[Client][UI] Lobby changed to canvas #%d\n", currentCanvasId);
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
            printf("[Client][UI] Lobby changed to canvas #%d\n", currentCanvasId);
        }
    }
};

/* ============================================================================
   SAVE BUTTON
   ============================================================================ */

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
        printf("[Client][UI] Save button clicked\n");
        send_tcp_save();
    }
};

/* ============================================================================
   LAYER BUTTONS (Right side of screen)
   ============================================================================ */

class LayerButton : public Button {
public:
    int layerId;
    
    virtual void Draw(SDL_Renderer* renderer) override {
        if (currentLayerId == layerId) {
            SDL_SetRenderDrawColor(renderer, 100, 200, 100, 255);  // Green = selected
        } else {
            SDL_SetRenderDrawColor(renderer, 150, 150, 150, 255);  // Grey = not selected
        }
        SDL_Rect rect = {x, y, w, h};
        SDL_RenderFillRect(renderer, &rect);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderDrawRect(renderer, &rect);
    }
    
    virtual void Click() override {
        currentLayerId = layerId;
        printf("[Client][UI] Layer switched to %d\n", layerId);
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
        printf("[Client][UI] Add layer button clicked\n");
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
            printf("[Client][UI] Delete layer button clicked for layer %d\n", currentLayerId);
            send_tcp_delete_layer(currentLayerId);
            // layerCount and currentLayerId will be updated when server broadcasts MSG_LAYER_DEL
        } else {
            printf("[Client][UI] Cannot delete layer: must keep at least 1 drawable layer\n");
        }
    }
};

/* ============================================================================
   UNDO/REDO BUTTONS (Bottom of screen)
   ============================================================================ */

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

/* ============================================================================
   HELPER FUNCTIONS
   ============================================================================ */

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

/* ============================================================================
   MAIN UI FUNCTIONS
   ============================================================================ */

// Button indices
#define SAVE_BTN_IDX 10
#define ADD_LAYER_BTN_IDX 11
#define DEL_LAYER_BTN_IDX 12
#define UNDO_BTN_IDX 13
#define REDO_BTN_IDX 14
#define LAYER_BUTTONS_START 15

inline void SetupUI() {
    printf("[Client][UI] Setting up UI buttons...\n");
    
    // === LOGIN SCREEN BUTTONS (indices 0, 1, 2) ===
    LoginButton* loginBtn = new LoginButton();
    loginBtn->x = UI_WIDTH/2 - 75; loginBtn->y = UI_HEIGHT/2 - 25; loginBtn->w = 150; loginBtn->h = 50;
    buttons.push_back(loginBtn);

    LobbyLeftButton* lobbyLeft = new LobbyLeftButton();
    lobbyLeft->x = UI_WIDTH/2 - 120; lobbyLeft->y = UI_HEIGHT/2 - 25; lobbyLeft->w = 40; lobbyLeft->h = 50;
    buttons.push_back(lobbyLeft);

    LobbyRightButton* lobbyRight = new LobbyRightButton();
    lobbyRight->x = UI_WIDTH/2 + 80; lobbyRight->y = UI_HEIGHT/2 - 25; lobbyRight->w = 40; lobbyRight->h = 50;
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

    BrushButton* b1 = new BrushButton(); b1->x = 120; b1->y = 10; b1->w = 40; b1->h = 40; b1->brushId = 0;
    buttons.push_back(b1);
    BrushButton* b2 = new BrushButton(); b2->x = 120; b2->y = 60; b2->w = 40; b2->h = 40; b2->brushId = 1;
    buttons.push_back(b2);
    BrushButton* b3 = new BrushButton(); b3->x = 120; b3->y = 110; b3->w = 40; b3->h = 40; b3->brushId = 2;
    buttons.push_back(b3);

    SizeUpButton* sizeUp = new SizeUpButton(); sizeUp->x = 170; sizeUp->y = 10; sizeUp->w = 30; sizeUp->h = 30;
    buttons.push_back(sizeUp);
    SizeDownButton* sizeDown = new SizeDownButton(); sizeDown->x = 170; sizeDown->y = 50; sizeDown->w = 30; sizeDown->h = 30;
    buttons.push_back(sizeDown);

    // === SAVE BUTTON (index 10) - Top center ===
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

    // === LAYER BUTTONS (indices 15+) - Will be created dynamically ===
    // Initial layer buttons for layers 1-2
    for (int i = 1; i <= 2; i++) {
        LayerButton* lb = new LayerButton();
        lb->x = UI_WIDTH - 45; lb->y = 40 + (i-1) * 35; lb->w = 35; lb->h = 30;
        lb->layerId = i;
        buttons.push_back(lb);
    }

    printf("[Client][UI] UI setup complete: %zu buttons\n", buttons.size());
}

inline void UpdateLayerButtons() {
    // Remove old layer buttons (keep indices 0-14)
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

inline void draw_ui(SDL_Renderer* renderer) {
    SDL_SetRenderDrawColor(renderer, 200, 200, 200, 255);
    SDL_RenderClear(renderer);

    if (!loggedin) {
        SDL_SetRenderDrawColor(renderer, 50, 50, 80, 255);
        SDL_Rect bg = {0, 0, UI_WIDTH, UI_HEIGHT};
        SDL_RenderFillRect(renderer, &bg);
        
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        draw_digit(renderer, currentCanvasId, UI_WIDTH/2 - 10, UI_HEIGHT/2 - 80, 20);
        SDL_RenderDrawLine(renderer, UI_WIDTH/2 - 40, UI_HEIGHT/2 - 100, UI_WIDTH/2 + 40, UI_HEIGHT/2 - 100);
        
        if (buttons.size() > 0) buttons[0]->Draw(renderer);
        if (buttons.size() > 1) buttons[1]->Draw(renderer);
        if (buttons.size() > 2) buttons[2]->Draw(renderer);
        
        SDL_RenderPresent(renderer);
        return;
    }

    SDL_RenderCopy(renderer, canvasTexture, NULL, NULL);

    // Draw tool buttons (indices 3-9)
    for (size_t i = 3; i < 10 && i < buttons.size(); i++) {
        buttons[i]->Draw(renderer);
    }
    
    // Draw save button (index 10)
    if (buttons.size() > SAVE_BTN_IDX) buttons[SAVE_BTN_IDX]->Draw(renderer);
    
    // Draw layer control buttons (indices 11-12)
    if (buttons.size() > ADD_LAYER_BTN_IDX) buttons[ADD_LAYER_BTN_IDX]->Draw(renderer);
    if (buttons.size() > DEL_LAYER_BTN_IDX) buttons[DEL_LAYER_BTN_IDX]->Draw(renderer);
    
    // Draw undo/redo buttons (indices 13-14)
    if (buttons.size() > UNDO_BTN_IDX) buttons[UNDO_BTN_IDX]->Draw(renderer);
    if (buttons.size() > REDO_BTN_IDX) buttons[REDO_BTN_IDX]->Draw(renderer);
    
    // Draw layer buttons (indices 15+)
    for (size_t i = LAYER_BUTTONS_START; i < buttons.size(); i++) {
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

    SDL_RenderPresent(renderer);
}

inline bool handle_login_screen_click(int x, int y) {
    for (int i = 0; i <= 2 && i < (int)buttons.size(); i++) {
        Button* btn = buttons[i];
        if (x >= btn->x && x < btn->x + btn->w && y >= btn->y && y < btn->y + btn->h) {
            btn->Click();
            return true;
        }
    }
    return false;
}

inline bool handle_canvas_ui_click(int x, int y) {
    // Check tool buttons (3-9)
    for (size_t i = 3; i < 10 && i < buttons.size(); i++) {
        Button* btn = buttons[i];
        if (x >= btn->x && x < btn->x + btn->w && y >= btn->y && y < btn->y + btn->h) {
            btn->Click();
            return true;
        }
    }
    // Check save button (10)
    if (buttons.size() > SAVE_BTN_IDX) {
        Button* btn = buttons[SAVE_BTN_IDX];
        if (x >= btn->x && x < btn->x + btn->w && y >= btn->y && y < btn->y + btn->h) {
            btn->Click();
            return true;
        }
    }
    // Check layer control buttons (11-12)
    for (size_t i = ADD_LAYER_BTN_IDX; i <= DEL_LAYER_BTN_IDX && i < buttons.size(); i++) {
        Button* btn = buttons[i];
        if (x >= btn->x && x < btn->x + btn->w && y >= btn->y && y < btn->y + btn->h) {
            btn->Click();
            UpdateLayerButtons();
            return true;
        }
    }
    // Check undo/redo buttons (13-14)
    for (size_t i = UNDO_BTN_IDX; i <= REDO_BTN_IDX && i < buttons.size(); i++) {
        Button* btn = buttons[i];
        if (x >= btn->x && x < btn->x + btn->w && y >= btn->y && y < btn->y + btn->h) {
            btn->Click();
            return true;
        }
    }
    // Check layer buttons (15+)
    for (size_t i = LAYER_BUTTONS_START; i < buttons.size(); i++) {
        Button* btn = buttons[i];
        if (x >= btn->x && x < btn->x + btn->w && y >= btn->y && y < btn->y + btn->h) {
            btn->Click();
            return true;
        }
    }
    return false;
}

#endif // UI_H
