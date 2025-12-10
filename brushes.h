#ifndef BRUSHES_H
#define BRUSHES_H

#ifdef SERVER_SIDE  //server used to use sdl2 too only for SDL_color but the uni server said no :C
    #include <cstdint>
    struct Pixel { uint8_t r, g, b, a; };
#else
    #include <SDL2/SDL.h>
    typedef SDL_Color Pixel;
#endif

#include <functional>
#include <cmath>

// Abstract base class for Brushes
// Will add way more in the final version
class Brush {
public:
    int size = 5;
    virtual ~Brush() = default;
    
    // Paint function: takes coordinates, color, size, and a callback to set a pixel
    // The callback allows this to be used with both Server (array) and Client (renderer/texture)
    virtual void paint(int x, int y, Pixel color, int size, std::function<void(int, int, Pixel)> setPixel) = 0;
};

// Derived: Round Brush (Standard)
class RoundBrush : public Brush {
public:
    void paint(int x, int y, Pixel color, int size, std::function<void(int, int, Pixel)> setPixel) override {
        int r = size / 2;
        if (r < 1) {
            setPixel(x, y, color);
            return;
        }
        for (int i = -r; i <= r; i++) {
            for (int j = -r; j <= r; j++) {
                if (i*i + j*j <= r*r) {
                    setPixel(x + i, y + j, color);
                }
            }
        }
    }
};

// Derived: Square Brush
class SquareBrush : public Brush {
public:
    void paint(int x, int y, Pixel color, int size, std::function<void(int, int, Pixel)> setPixel) override {
        int r = size / 2;
        for (int i = -r; i <= r; i++) {
            for (int j = -r; j <= r; j++) {
                setPixel(x + i, y + j, color);
            }
        }
    }
};

class HardEraserBrush : public Brush {
public:
    void paint(int x, int y, Pixel color, int size, std::function<void(int, int, Pixel)> setPixel) override {
        (void)color; // unused
        int r = size / 2;
        for (int i = -r; i <= r; i++) {
            for (int j = -r; j <= r; j++) {
                // Set to white (paper color) for display, transparent for layer storage
                Pixel erased = {255, 255, 255, 0};  // alpha=0 means erased on layer
                setPixel(x + i, y + j, erased);
            }
        }
    }
};

#endif
