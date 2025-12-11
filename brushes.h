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
    int opacity = 255; // 0-255
    virtual ~Brush() = default;
    
    // Paint function: takes coordinates, color, size, pressure, and a callback to set a pixel
    virtual void paint(int x, int y, Pixel color, int size, int pressure, std::function<void(int, int, Pixel)> setPixel) = 0;
};

// Derived: Round Brush (Standard)
class RoundBrush : public Brush {
public:
    void paint(int x, int y, Pixel color, int size, int pressure, std::function<void(int, int, Pixel)> setPixel) override {
        (void)pressure; // Unused for standard round brush
        color.a = (uint8_t)((color.a * opacity) / 255);
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
    void paint(int x, int y, Pixel color, int size, int pressure, std::function<void(int, int, Pixel)> setPixel) override {
        (void)pressure;
        color.a = (uint8_t)((color.a * opacity) / 255);
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
    void paint(int x, int y, Pixel color, int size, int pressure, std::function<void(int, int, Pixel)> setPixel) override {
        (void)color; // unused
        (void)pressure;
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

// Derived: Pressure Brush
class PressureBrush : public Brush {
public:
    void paint(int x, int y, Pixel color, int size, int pressure, std::function<void(int, int, Pixel)> setPixel) override {
        // Calculate effective size based on pressure (0-255)
        // Use float for smoother size calculation
        float p = pressure / 255.0f;
        float effectiveSizeFloat = size * sqrt(p);
        int effectiveSize = (int)effectiveSizeFloat;
        
        // Apply brush opacity
        color.a = (uint8_t)((color.a * opacity) / 255);
        
        if (effectiveSize < 1) {
            // If pressure is very low but non-zero, draw a single pixel with reduced alpha
            if (pressure > 0) {
                Pixel faint = color;
                faint.a = (uint8_t)(color.a * p); 
                setPixel(x, y, faint);
            }
            return;
        }
        
        int r = effectiveSize / 2;
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

// Derived: Airbrush (Opacity/Density varies with pressure, Size varies slightly)
class Airbrush : public Brush {
public:
    void paint(int x, int y, Pixel color, int size, int pressure, std::function<void(int, int, Pixel)> setPixel) override {
        // Size varies slightly: 80% base + 20% pressure
        float pressureFactor = pressure / 255.0f;
        int effectiveSize = (int)(size * (0.8f + 0.2f * pressureFactor));
        if (effectiveSize < 1) effectiveSize = 1;
        
        // Apply brush opacity
        color.a = (uint8_t)((color.a * opacity) / 255);
        
        int r = effectiveSize; // Use full size as radius for softer feel
        
        for (int i = -r; i <= r; i++) {
            for (int j = -r; j <= r; j++) {
                float distSq = (float)(i*i + j*j);
                float radiusSq = (float)(r*r);
                
                if (distSq <= radiusSq) {
                    float dist = sqrt(distSq);
                    // Falloff: 1.0 at center, 0.0 at edge
                    float falloff = 1.0f - (dist / r);
                    if (falloff < 0) falloff = 0;
                    
                    // Probability based on falloff and pressure
                    // Higher pressure = denser dots
                    float probability = falloff * pressureFactor;
                    
                    // Stochastic sampling (dithering)
                    if ((rand() % 1000) / 1000.0f < probability) {
                        setPixel(x + i, y + j, color);
                    }
                }
            }
        }
    }
};

#endif
