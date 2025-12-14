#ifndef BRUSHES_H
#define BRUSHES_H

#ifdef SERVER_SIDE
    #include <cstdint>
    struct Pixel { uint8_t r, g, b, a; };
#else
    #include <SDL2/SDL.h>
    typedef SDL_Color Pixel;
#endif

#include <functional>
#include <cmath>
#include <cstdlib> 

class Brush {
public:
    int size = 5;
    int opacity = 255; // 0-255
    virtual ~Brush() = default;
    virtual void paint(int x, int y, Pixel color, int size, int pressure, std::function<void(int, int, Pixel)> setPixel) = 0;
};

class RoundBrush : public Brush {
public:
    void paint(int x, int y, Pixel color, int size, int pressure, std::function<void(int, int, Pixel)> setPixel) override {
        (void)pressure; 
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
        (void)color; (void)pressure;
        int r = size / 2;
        for (int i = -r; i <= r; i++) {
            for (int j = -r; j <= r; j++) {
                Pixel erased = {255, 255, 255, 0}; 
                setPixel(x + i, y + j, erased);
            }
        }
    }
};

// ================= MODIFIED BRUSHES START HERE =================

class PressureBrush : public Brush {
public:
    void paint(int x, int y, Pixel color, int maxSize, int pressure, std::function<void(int, int, Pixel)> setPixel) override {
        float p = pressure / 255.0f;
        
        // 1. Opacity Curve (Square Root)
        // Ramps up fast: 10% pressure -> 45% opacity
        float opacityCurve = 0.2f + 0.8f * sqrt(p);
        if (opacityCurve > 1.0f) opacityCurve = 1.0f;
        float baseAlpha = (color.a * opacity / 255.0f) * opacityCurve;
        
        // 2. Size Influence (30% Minimum)
        // 30% static base + 70% dynamic pressure
        float effectiveDiameter = maxSize * (0.3f + 0.7f * p);
        float radius = effectiveDiameter / 2.0f; 
        if (radius < 0.5f) radius = 0.5f;

        int range = (int)ceil(radius) + 1;

        for (int i = -range; i <= range; i++) {
            for (int j = -range; j <= range; j++) {
                float dist = sqrt(i*i + j*j);
                
                // 3. Feathering / Soft Edge
                // Instead of 1 pixel AA, we fade over the last 1.5 pixels.
                // This hides the "stepping" artifact when size changes slightly.
                float featherRange = 1.5f; 
                float delta = (radius - dist + (featherRange / 2.0f)) / featherRange;
                
                // Clamp
                if (delta < 0.0f) delta = 0.0f;
                if (delta > 1.0f) delta = 1.0f;

                if (delta > 0.0f) {
                    Pixel px = color;
                    // Apply feathering to alpha
                    px.a = (uint8_t)(baseAlpha * delta);
                    
                    if (px.a > 0) {
                        setPixel(x + i, y + j, px);
                    }
                }
            }
        }
    }
};

class Airbrush : public Brush {
public:
    void paint(int x, int y, Pixel color, int size, int pressure, std::function<void(int, int, Pixel)> setPixel) override {
        float p = pressure / 255.0f;
        
        // 1. Size Influence
        int effectiveSize = (int)(size * (0.7f + 0.3f * p));
        if (effectiveSize < 1) effectiveSize = 1;
        
        // 2. Pressure Opacity (More Transparent)
        // Range reduced to 5% - 60% (was 10-100%) to allow building up layers
        float pressureAlphaMod = 0.05f + 0.55f * p; 
        
        int r = effectiveSize; 
        
        for (int i = -r; i <= r; i++) {
            for (int j = -r; j <= r; j++) {
                float dist = sqrt(i*i + j*j);
                
                if (dist <= r) {
                    // 3. Radial Falloff (Soft Center)
                    float falloff = 1.0f - (dist / r);
                    
                    // Smooth the falloff (standard cubic ease-out) for softer look
                    falloff = falloff * falloff; 
                    
                    float finalAlphaFloat = (color.a * opacity / 255.0f) * pressureAlphaMod * falloff;
                    
                    Pixel px = color;
                    px.a = (uint8_t)finalAlphaFloat;
                    
                    if (px.a > 0) {
                        setPixel(x + i, y + j, px);
                    }
                }
            }
        }
    }
};

#endif