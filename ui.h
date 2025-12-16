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
extern SDL_Texture* signatureTexture; 
extern SDL_Rect signatureRect;
extern SDL_Texture* menuTexture; 
extern bool isDrawingSignature;

struct RemoteCursor {
    int x, y;
    SDL_Color color;
};
extern map<string, RemoteCursor> remote_cursors;

extern void send_tcp_login(const char* username);
extern void send_tcp_save();
extern void send_tcp_add_layer(int layer_id = 0);
extern void send_tcp_delete_layer(int layer_id);
extern void send_tcp_reorder_layer(int old_idx, int new_idx);
extern void UpdateLayerButtons();
extern void perform_undo();
extern void perform_redo();
extern void save_undo_state();
extern void download_as_bmp();
extern vector<CanvasSnapshot*> redoStack;

#define UI_WIDTH 640
#define UI_HEIGHT 480

// Drag state globals
int dragLayerId = -1;
int dragStartY = -1;
int dragCurrentY = -1;

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

// --- HELPERS ---

// Standard digit drawing (For Layer Numbers)
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

// Thick digit drawing (For Main Menu Canvas ID)
inline void draw_digit_thick(SDL_Renderer* renderer, int digit, int x, int y, int size) {
    int w = size, h = size * 2;
    bool segs[10][7] = {
        {1,1,1,1,1,1,0}, {0,1,1,0,0,0,0}, {1,1,0,1,1,0,1}, {1,1,1,1,0,0,1}, {0,1,1,0,0,1,1},
        {1,0,1,1,0,1,1}, {1,0,1,1,1,1,1}, {1,1,1,0,0,0,0}, {1,1,1,1,1,1,1}, {1,1,1,1,0,1,1}
    };
    if (digit < 0 || digit > 9) return;
    auto draw_thick = [&](int x1, int y1, int x2, int y2) {
        SDL_RenderDrawLine(renderer, x1, y1, x2, y2);
        if (x1 == x2) { SDL_RenderDrawLine(renderer, x1-1, y1, x2-1, y2); SDL_RenderDrawLine(renderer, x1+1, y1, x2+1, y2); } 
        else { SDL_RenderDrawLine(renderer, x1, y1-1, x2, y2-1); SDL_RenderDrawLine(renderer, x1, y1+1, x2, y2+1); }
    };
    if (segs[digit][0]) draw_thick(x, y, x+w, y);
    if (segs[digit][1]) draw_thick(x+w, y, x+w, y+h/2);
    if (segs[digit][2]) draw_thick(x+w, y+h/2, x+w, y+h);
    if (segs[digit][3]) draw_thick(x, y+h, x+w, y+h);
    if (segs[digit][4]) draw_thick(x, y+h/2, x, y+h);
    if (segs[digit][5]) draw_thick(x, y, x, y+h/2);
    if (segs[digit][6]) draw_thick(x, y+h/2, x+w, y+h/2);
}

// Helper to check button clicks
inline bool point_in_button(Button* btn, int x, int y) {
    return x >= btn->x && x < btn->x + btn->w && y >= btn->y && y < btn->y + btn->h;
}


/*****************************************************************************
   MAIN MENU BUTTONS (INVISIBLE)
 *****************************************************************************/

class LoginButton : public Button {
public:
    void Draw(SDL_Renderer* r) override { /* Invisible for Main Menu Art */ }
    void Click() override { 
        ::printf("[Client][UI] Login button clicked for canvas #%d\n", currentCanvasId);
        send_tcp_login("Artist"); 
    }
};

class LobbyLeftButton : public Button {
public:
    void Draw(SDL_Renderer* r) override { /* Invisible */ }
    void Click() override { 
        if (currentCanvasId > 0) {
            currentCanvasId--;
            ::printf("[Client][UI] Lobby changed to canvas #%d\n", currentCanvasId);
        }
    }
};

class LobbyRightButton : public Button {
public:
    void Draw(SDL_Renderer* r) override { /* Invisible */ }
    void Click() override { 
        if (currentCanvasId < 99) {
            currentCanvasId++;
            ::printf("[Client][UI] Lobby changed to canvas #%d\n", currentCanvasId);
        }
    }
};


/*****************************************************************************
   CANVAS UI BUTTONS (OLD STYLE RESTORED)
 *****************************************************************************/

class ColorPicker : public Button {
public:
    virtual void Draw(SDL_Renderer* renderer) override {
        // Full Gradient Draw
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
        
        // Draw Large Preview Box (Right of Color Picker)
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
        else if (hue < 3) { r = 0; g = 255; b = x_val * 255; b = 0; }
        else if (hue < 4) { r = 0; g = x_val * 255; b = 255; }
        else if (hue < 5) { r = x_val * 255; g = 0; b = 255; }
        else { r = 255; g = 0; b = x_val * 255; }

        if (linkedPicker) {
            linkedPicker->color = {(Uint8)r, (Uint8)g, (Uint8)b, 255};
            userColor = linkedPicker->color; // Auto update active color
        }
    }
};

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

        // Icons
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        if (brushId == 0) { // Round
            for (int i = -3; i <= 3; i++) for (int j = -3; j <= 3; j++) if (i*i + j*j <= 9) SDL_RenderDrawPoint(renderer, x+w/2+i, y+h/2+j);
        } else if (brushId == 1) { // Square
            SDL_Rect r = {x+w/2-3, y+h/2-3, 7, 7}; SDL_RenderFillRect(renderer, &r);
        } else if (brushId == 2) { // Eraser
            SDL_SetRenderDrawColor(renderer, 255, 200, 200, 255);
            SDL_Rect r = {x+w/2-4, y+h/2-4, 9, 9}; SDL_RenderFillRect(renderer, &r);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255); SDL_RenderDrawRect(renderer, &r);
        } else if (brushId == 3) { // Pressure (P)
            int cx = x+w/2, cy = y+h/2;
            SDL_RenderDrawLine(renderer, cx-2, cy-4, cx-2, cy+4); SDL_RenderDrawLine(renderer, cx-2, cy-4, cx+2, cy-4);
            SDL_RenderDrawLine(renderer, cx+2, cy-4, cx+2, cy); SDL_RenderDrawLine(renderer, cx-2, cy, cx+2, cy);
        } else if (brushId == 4) { // Air (A)
            int cx = x+w/2, cy = y+h/2;
            SDL_RenderDrawLine(renderer, cx, cy-5, cx-3, cy+5); SDL_RenderDrawLine(renderer, cx, cy-5, cx+3, cy+5); SDL_RenderDrawLine(renderer, cx-2, cy, cx+2, cy);
        }
    }
    virtual void Click() override { currentBrushId = brushId; }
};

class SizeUpButton : public Button {
public:
    virtual void Draw(SDL_Renderer* renderer) override {
        SDL_SetRenderDrawColor(renderer, 100, 200, 100, 255);
        SDL_Rect rect = {x, y, w, h}; SDL_RenderFillRect(renderer, &rect);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255); SDL_RenderDrawRect(renderer, &rect);
        int cx = x+w/2, cy = y+h/2;
        SDL_RenderDrawLine(renderer, cx-5, cy, cx+5, cy); SDL_RenderDrawLine(renderer, cx, cy-5, cx, cy+5);
    }
    virtual void Click() override { if (currentBrushId >= 0 && currentBrushId < (int)availableBrushes.size()) availableBrushes[currentBrushId]->size++; }
};

class SizeDownButton : public Button {
public:
    virtual void Draw(SDL_Renderer* renderer) override {
        SDL_SetRenderDrawColor(renderer, 200, 100, 100, 255);
        SDL_Rect rect = {x, y, w, h}; SDL_RenderFillRect(renderer, &rect);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255); SDL_RenderDrawRect(renderer, &rect);
        int cx = x+w/2, cy = y+h/2;
        SDL_RenderDrawLine(renderer, cx-5, cy, cx+5, cy);
    }
    virtual void Click() override { 
        if (currentBrushId >= 0 && currentBrushId < (int)availableBrushes.size()) 
            if (availableBrushes[currentBrushId]->size > 1) availableBrushes[currentBrushId]->size--; 
    }
};

class SaveButton : public Button {
public:
    virtual void Draw(SDL_Renderer* renderer) override {
        SDL_SetRenderDrawColor(renderer, 50, 150, 50, 255);
        SDL_Rect rect = {x, y, w, h}; SDL_RenderFillRect(renderer, &rect);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        SDL_Rect inner = {x+8, y+5, w-16, h-15}; SDL_RenderDrawRect(renderer, &inner);
        SDL_Rect slot = {x+12, y+5, w-24, 6}; SDL_RenderFillRect(renderer, &slot);
    }
    virtual void Click() override { send_tcp_save(); }
};

class LayerButton : public Button {
public:
    int layerId;
    virtual void Draw(SDL_Renderer* renderer) override {
        int drawY = (dragLayerId == layerId) ? dragCurrentY - h/2 : y;
        if (currentLayerId == layerId) SDL_SetRenderDrawColor(renderer, 100, 200, 100, 255);
        else SDL_SetRenderDrawColor(renderer, 150, 150, 150, 255);
        SDL_Rect rect = {x, drawY, w, h}; SDL_RenderFillRect(renderer, &rect);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255); SDL_RenderDrawRect(renderer, &rect);
        draw_digit(renderer, layerDisplayIds[layerId], x + w/2, drawY + h/2, 10);
    }
    virtual void Click() override { currentLayerId = layerId; }
};

class AddLayerButton : public Button {
public:
    virtual void Draw(SDL_Renderer* renderer) override {
        SDL_SetRenderDrawColor(renderer, 100, 200, 100, 255);
        SDL_Rect rect = {x, y, w, h}; SDL_RenderFillRect(renderer, &rect);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        int cx = x+w/2, cy = y+h/2;
        SDL_RenderDrawLine(renderer, cx-4, cy, cx+4, cy); SDL_RenderDrawLine(renderer, cx, cy-4, cx, cy+4);
    }
    virtual void Click() override { save_undo_state(); send_tcp_add_layer(); }
};

class DeleteLayerButton : public Button {
public:
    virtual void Draw(SDL_Renderer* renderer) override {
        SDL_SetRenderDrawColor(renderer, 200, 100, 100, 255);
        SDL_Rect rect = {x, y, w, h}; SDL_RenderFillRect(renderer, &rect);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        int cx = x+w/2, cy = y+h/2; SDL_RenderDrawLine(renderer, cx-4, cy, cx+4, cy);
    }
    virtual void Click() override { if (layerCount > 2 && currentLayerId > 0) { save_undo_state(); send_tcp_delete_layer(currentLayerId); } }
};

class UndoButton : public Button {
public:
    virtual void Draw(SDL_Renderer* renderer) override {
        SDL_SetRenderDrawColor(renderer, 80, 80, 120, 255);
        SDL_Rect rect = {x, y, w, h}; SDL_RenderFillRect(renderer, &rect);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255); SDL_RenderDrawRect(renderer, &rect);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        int cx = x + w/2, cy = y + h/2;
        SDL_RenderDrawLine(renderer, cx + 8, cy, cx - 4, cy); // Arrow
        SDL_RenderDrawLine(renderer, cx - 4, cy, cx + 2, cy - 5);
        SDL_RenderDrawLine(renderer, cx - 4, cy, cx + 2, cy + 5);
    }
    virtual void Click() override { perform_undo(); }
};

class RedoButton : public Button {
public:
    virtual void Draw(SDL_Renderer* renderer) override {
        if (redoStack.empty()) return;
        SDL_SetRenderDrawColor(renderer, 80, 80, 120, 255);
        SDL_Rect rect = {x, y, w, h}; SDL_RenderFillRect(renderer, &rect);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255); SDL_RenderDrawRect(renderer, &rect);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        int cx = x + w/2, cy = y + h/2;
        SDL_RenderDrawLine(renderer, cx - 8, cy, cx + 4, cy);
        SDL_RenderDrawLine(renderer, cx + 4, cy, cx - 2, cy - 5);
        SDL_RenderDrawLine(renderer, cx + 4, cy, cx - 2, cy + 5);
    }
    virtual void Click() override { perform_redo(); }
};

class DownloadButton : public Button {
public:
    virtual void Draw(SDL_Renderer* renderer) override {
        SDL_SetRenderDrawColor(renderer, 150, 75, 0, 255);
        SDL_Rect rect = {x, y, w, h}; SDL_RenderFillRect(renderer, &rect);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        SDL_Rect inner = {x+10, y+5, w-20, h-15}; SDL_RenderDrawRect(renderer, &inner);
        int cx = x + w/2, cy = y + h/2;
        SDL_RenderDrawLine(renderer, cx, cy - 5, cx, cy + 5);
        SDL_RenderDrawLine(renderer, cx - 5, cy + 2, cx, cy + 5);
        SDL_RenderDrawLine(renderer, cx + 5, cy + 2, cx, cy + 5);
    }
    virtual void Click() override { download_as_bmp(); }
};

class EyedropperButton : public Button {
public:
    virtual void Draw(SDL_Renderer* renderer) override {
        if (isEyedropping) SDL_SetRenderDrawColor(renderer, 100, 100, 255, 255);
        else SDL_SetRenderDrawColor(renderer, 200, 200, 200, 255);
        SDL_Rect rect = {x, y, w, h}; SDL_RenderFillRect(renderer, &rect);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255); SDL_RenderDrawRect(renderer, &rect);
        int cx = x + w/2, cy = y + h/2;
        SDL_RenderDrawLine(renderer, cx-3, cy-3, cx+3, cy+3);
        SDL_RenderDrawLine(renderer, cx-3, cy-3, cx, cy-6);
        SDL_RenderDrawLine(renderer, cx+3, cy+3, cx+6, cy);
        SDL_RenderDrawLine(renderer, cx, cy-6, cx+6, cy);
    }
    virtual void Click() override { isEyedropping = !isEyedropping; }
};

// Indices
#define SAVE_BTN_IDX 13
#define ADD_LAYER_BTN_IDX 14
#define DEL_LAYER_BTN_IDX 15
#define UNDO_BTN_IDX 16
#define REDO_BTN_IDX 17
#define EYEDROPPER_BTN_IDX 18
#define LAYER_BUTTONS_START 19

inline void UpdateLayerButtons() {
    while (buttons.size() > LAYER_BUTTONS_START) { delete buttons.back(); buttons.pop_back(); }
    int numDrawableLayers = layerCount - 1;
    for (int i = numDrawableLayers; i >= 1; i--) {
        LayerButton* lb = new LayerButton();
        lb->x = UI_WIDTH - 45; 
        lb->y = 40 + (numDrawableLayers - i) * 35; 
        lb->w = 35; lb->h = 30;
        lb->layerId = i;
        buttons.push_back(lb);
    }
}

inline void SetupUI() {
    for (auto* b : buttons) delete b;
    buttons.clear();

    // === PART 1: MAIN MENU (KEEP YOUR HAND DRAWN ART) ===
    // Coordinates matched to your art
    LoginButton* loginBtn = new LoginButton();
    loginBtn->w = 220; loginBtn->h = 100;
    loginBtn->x = (UI_WIDTH / 2) - (loginBtn->w / 2); loginBtn->y = 330;
    buttons.push_back(loginBtn); // 0

    LobbyLeftButton* leftBtn = new LobbyLeftButton();
    leftBtn->x = 50; leftBtn->y = 30; leftBtn->w = 200; leftBtn->h = 80;
    buttons.push_back(leftBtn); // 1

    LobbyRightButton* rightBtn = new LobbyRightButton();
    rightBtn->x = 370; rightBtn->y = 30; rightBtn->w = 200; rightBtn->h = 80;
    buttons.push_back(rightBtn); // 2

    // === PART 2: CANVAS UI (RESTORE THE OLD STYLE) ===
    // Tools (Indices 3+)
    ColorPicker* colp = new ColorPicker();
    colp->x = 10; colp->y = 10; colp->w = 100; colp->h = 100;
    colp->color = {255, 0, 0, 255}; buttons.push_back(colp);

    HuePicker* hp = new HuePicker();
    hp->x = 10; hp->y = 120; hp->w = 100; hp->h = 20;
    hp->linkedPicker = colp; buttons.push_back(hp);

    SizeUpButton* sizeUp = new SizeUpButton(); sizeUp->x = 10; sizeUp->y = 150; sizeUp->w = 30; sizeUp->h = 30; buttons.push_back(sizeUp);
    SizeDownButton* sizeDown = new SizeDownButton(); sizeDown->x = 45; sizeDown->y = 150; sizeDown->w = 30; sizeDown->h = 30; buttons.push_back(sizeDown);
    DownloadButton* downloadBtn = new DownloadButton(); downloadBtn->x = 80; downloadBtn->y = 150; downloadBtn->w = 30; downloadBtn->h = 30; buttons.push_back(downloadBtn);

    BrushButton* b1 = new BrushButton(); b1->x = 10; b1->y = 190; b1->w = 30; b1->h = 30; b1->brushId = 0; buttons.push_back(b1);
    BrushButton* b2 = new BrushButton(); b2->x = 10; b2->y = 225; b2->w = 30; b2->h = 30; b2->brushId = 1; buttons.push_back(b2);
    BrushButton* b3 = new BrushButton(); b3->x = 10; b3->y = 260; b3->w = 30; b3->h = 30; b3->brushId = 2; buttons.push_back(b3);
    BrushButton* b4 = new BrushButton(); b4->x = 10; b4->y = 295; b4->w = 30; b4->h = 30; b4->brushId = 3; buttons.push_back(b4);
    BrushButton* b5 = new BrushButton(); b5->x = 10; b5->y = 330; b5->w = 30; b5->h = 30; b5->brushId = 4; buttons.push_back(b5);

    SaveButton* saveBtn = new SaveButton(); saveBtn->x = UI_WIDTH/2 - 25; saveBtn->y = 10; saveBtn->w = 50; saveBtn->h = 30; buttons.push_back(saveBtn);

    AddLayerButton* addLayer = new AddLayerButton(); addLayer->x = UI_WIDTH - 50; addLayer->y = 10; addLayer->w = 20; addLayer->h = 20; buttons.push_back(addLayer);
    DeleteLayerButton* delLayer = new DeleteLayerButton(); delLayer->x = UI_WIDTH - 25; delLayer->y = 10; delLayer->w = 20; delLayer->h = 20; buttons.push_back(delLayer);

    UndoButton* undoBtn = new UndoButton(); undoBtn->x = UI_WIDTH/2 - 70; undoBtn->y = UI_HEIGHT - 40; undoBtn->w = 60; undoBtn->h = 30; buttons.push_back(undoBtn);
    RedoButton* redoBtn = new RedoButton(); redoBtn->x = UI_WIDTH/2 + 10; redoBtn->y = UI_HEIGHT - 40; redoBtn->w = 60; redoBtn->h = 30; buttons.push_back(redoBtn);

    EyedropperButton* eyeBtn = new EyedropperButton(); eyeBtn->x = 120; eyeBtn->y = 10; eyeBtn->w = 30; eyeBtn->h = 30; buttons.push_back(eyeBtn);

    UpdateLayerButtons();
}

inline void draw_ui(SDL_Renderer* renderer, bool uiVisible, std::function<void(SDL_Renderer*)> postCanvasCallback = nullptr) {
    SDL_SetRenderDrawColor(renderer, 200, 200, 200, 255);
    SDL_RenderClear(renderer);

    if (!loggedin) {
        // --- DRAW MAIN MENU (KEEP EXACTLY AS IS) ---
        if (menuTexture) {
            SDL_SetTextureBlendMode(menuTexture, SDL_BLENDMODE_NONE);
            SDL_RenderCopy(renderer, menuTexture, NULL, NULL);
        } else {
            SDL_SetRenderDrawColor(renderer, 50, 50, 50, 255);
            SDL_RenderClear(renderer);
        }

        if (signatureTexture) {
            SDL_SetTextureBlendMode(signatureTexture, SDL_BLENDMODE_BLEND);
            SDL_RenderCopy(renderer, signatureTexture, NULL, &signatureRect);
        }

        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        int digit1 = currentCanvasId / 10;
        int digit2 = currentCanvasId % 10;
        draw_digit_thick(renderer, digit1, UI_WIDTH/2 - 25, 55, 20); 
        draw_digit_thick(renderer, digit2, UI_WIDTH/2 + 5, 55, 20);
        
        SDL_RenderPresent(renderer);
        return;
    }

    // --- DRAW CANVAS UI (RESTORED OLD STYLE) ---
    // Draw Canvas Layers
    if (canvasTexture) SDL_RenderCopy(renderer, canvasTexture, NULL, NULL);
    for (int i = 0; i < layerCount; i++) {
        if (layerTextures[i]) {
            SDL_SetTextureAlphaMod(layerTextures[i], layerOpacity[i]);
            SDL_RenderCopy(renderer, layerTextures[i], NULL, NULL);
        }
    }
    
    if (postCanvasCallback) {
        postCanvasCallback(renderer);
    }

    // Draw Remote Cursors
    for (auto& [uid, c] : remote_cursors) {
        SDL_SetRenderDrawColor(renderer, c.color.r, c.color.g, c.color.b, 255);
        SDL_Rect cr = {c.x - 5, c.y - 5, 10, 10}; SDL_RenderDrawRect(renderer, &cr);
    }

    // Draw UI Buttons (Skip first 3 lobby buttons)
    if (uiVisible) {
        for (size_t i = 3; i < buttons.size(); i++) {
            buttons[i]->Draw(renderer);
        }
    }

    // Brush Cursor
    int mx, my;
    SDL_GetMouseState(&mx, &my);
    if (!isEyedropping && mx > 150 && mx < 550) { // Rough bounds check
        if (currentBrushId >= 0 && currentBrushId < (int)availableBrushes.size()) {
            int size = availableBrushes[currentBrushId]->size;
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderDrawLine(renderer, mx - 5, my, mx + 5, my);
            SDL_RenderDrawLine(renderer, mx, my - 5, mx, my + 5);
        }
    }

    SDL_RenderPresent(renderer);
}

inline bool handle_login_screen_click(int x, int y) {
    // Check signature box
    if (x >= signatureRect.x && x < signatureRect.x + signatureRect.w &&
        y >= signatureRect.y && y < signatureRect.y + signatureRect.h) {
        isDrawingSignature = true;
        return true;
    }
    // Only check 0-2 (Lobby Buttons)
    for (size_t i = 0; i <= 2; i++) {
        if (point_in_button(buttons[i], x, y)) {
            buttons[i]->Click();
            return true;
        }
    }
    return false;
}

inline bool handle_canvas_ui_click(int x, int y) {
    // Skip 0-2 (Lobby buttons)
    for (size_t i = 3; i < buttons.size(); i++) {
        Button* btn = buttons[i];
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

#endif
