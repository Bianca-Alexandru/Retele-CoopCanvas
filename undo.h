#ifndef UNDO_H
#define UNDO_H

#include <vector>
#include <cstring>
#include <cstdint>
#include <SDL2/SDL.h>
#include <cstdio>

// Forward declarations of functions we need to call from client.cpp
#define MAX_LAYERS 15
extern uint8_t* layers[MAX_LAYERS]; 
extern void send_tcp_layer_sync(int layer_id); 
extern void send_tcp_add_layer(int layer_id); // Readding a deleted layer
extern void send_tcp_delete_layer(int layer_id);
extern void send_tcp_reorder_layer(int old_idx, int new_idx);
extern void move_layer_local(int layer_id, int dx, int dy); // From move logic
extern void send_tcp_layer_move(int layer_id, int dx, int dy); // Helper needed

// --- ABSTRACT BASE CLASS ---
class Command {
public:
    Uint32 timestamp; // Added timestamp for expiration
    Command() { timestamp = SDL_GetTicks(); }
    virtual ~Command() {}
    virtual void undo() = 0;
    virtual void redo() = 0;
    
    // Helper to check if a layer exists before trying to modify it
    bool layerExists(int id) {
        return (id >= 0 && id < MAX_LAYERS && layers[id] != nullptr); 
    }
};

// --- 1. PAINT COMMAND (For Brush Strokes) ---
class PaintCommand : public Command {
    int layerId;
    uint8_t* pixelsBefore;
    uint8_t* pixelsAfter;
    int width, height;

public:
    PaintCommand(int id, int w, int h) : layerId(id), width(w), height(h) {
        pixelsBefore = new uint8_t[w * h * 4];
        pixelsAfter = new uint8_t[w * h * 4];
    }

    ~PaintCommand() {
        delete[] pixelsBefore;
        delete[] pixelsAfter;
    }

    void captureBefore() {
        if (layerExists(layerId)) 
            memcpy(pixelsBefore, layers[layerId], width * height * 4);
    }

    void captureAfter() {
        if (layerExists(layerId)) 
            memcpy(pixelsAfter, layers[layerId], width * height * 4);
    }

    void undo() override {
        if (!layerExists(layerId)) return;
        // Restore local memory
        memcpy(layers[layerId], pixelsBefore, width * height * 4);
        // Sync with server (reuse existing standard message)
        send_tcp_layer_sync(layerId); 
    }

    void redo() override {
        if (!layerExists(layerId)) return;
        memcpy(layers[layerId], pixelsAfter, width * height * 4);
        send_tcp_layer_sync(layerId);
    }
};

// --- 2. LAYER MOVE COMMAND (Ctrl + Drag) ---
class MoveCommand : public Command {
    int layerId;
    int dx, dy; // Total movement

public:
    MoveCommand(int id, int _dx, int _dy) : layerId(id), dx(_dx), dy(_dy) {}

    void undo() override {
        if (!layerExists(layerId)) return;
        // Move back locally
        move_layer_local(layerId, -dx, -dy); 
        // Sync to server (reuse MSG_LAYER_MOVE logic)
        send_tcp_layer_move(layerId, -dx, -dy); 
    }

    void redo() override {
        if (!layerExists(layerId)) return;
        move_layer_local(layerId, dx, dy);
        send_tcp_layer_move(layerId, dx, dy);
    }
};

// --- 3. LAYER DELETE COMMAND ---
// When we delete a layer, we must save its pixels to restore it later.
class DeleteLayerCommand : public Command {
    int layerId;
    uint8_t* savedPixels;
    int width, height;

public:
    DeleteLayerCommand(int id, int w, int h) : layerId(id), width(w), height(h) {
        savedPixels = new uint8_t[w * h * 4];
        if (layerExists(id)) {
            memcpy(savedPixels, layers[id], w * h * 4);
        }
    }

    ~DeleteLayerCommand() {
        delete[] savedPixels;
    }

    void undo() override {
        // UNDOING a delete means ADDING it back
        send_tcp_add_layer(layerId); 
        
        if (layerExists(layerId)) {
             memcpy(layers[layerId], savedPixels, width * height * 4);
             send_tcp_layer_sync(layerId); // Upload the restored content
        }
    }

    void redo() override {
        send_tcp_delete_layer(layerId);
    }
};

// --- 4. LAYER ADD COMMAND ---
class AddLayerCommand : public Command {
    int layerId; // The ID that was added
public:
    AddLayerCommand(int id) : layerId(id) {}

    void undo() override {
        // UNDOING an add means DELETING it
        send_tcp_delete_layer(layerId);
    }

    void redo() override {
        // REDOING an add means ADDING it
        send_tcp_add_layer(layerId);
    }
};

#endif
