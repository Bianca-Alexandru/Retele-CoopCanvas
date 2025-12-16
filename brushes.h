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
#include <algorithm> // For std::min/max

// --- BASE CLASS ---
class Brush {
public:
    int size = 15;
    int opacity = 255; 
    virtual ~Brush() = default;
    virtual void paint(int x, int y, Pixel color, int size, int pressure, int angle, std::function<void(int, int, Pixel)> setPixel) = 0;
};

// --- EXISTING BRUSHES (Unchanged) ---
class RoundBrush : public Brush {
public:
    void paint(int x, int y, Pixel color, int size, int pressure, int angle, std::function<void(int, int, Pixel)> setPixel) override {
        (void)angle; (void)pressure;
        color.a = (uint8_t)((color.a * opacity) / 255);
        int r = size / 2;
        if (r < 1) { setPixel(x, y, color); return; }
        for (int i = -r; i <= r; i++) {
            for (int j = -r; j <= r; j++) {
                if (i*i + j*j <= r*r) setPixel(x + i, y + j, color);
            }
        }
    }
};

class SquareBrush : public Brush {
public:
    void paint(int x, int y, Pixel color, int size, int pressure, int angle, std::function<void(int, int, Pixel)> setPixel) override {
        (void)angle; (void)pressure;
        color.a = (uint8_t)((color.a * opacity) / 255);
        int r = size / 2;
        for (int i = -r; i <= r; i++) {
            for (int j = -r; j <= r; j++) setPixel(x + i, y + j, color);
        }
    }
};

class HardEraserBrush : public Brush {
public:
    void paint(int x, int y, Pixel color, int size, int pressure, int angle, std::function<void(int, int, Pixel)> setPixel) override {
        (void)color; (void)pressure; (void)angle;
        int r = size / 2;
        for (int i = -r; i <= r; i++) {
            for (int j = -r; j <= r; j++) {
                Pixel erased = {0, 0, 0, 0}; 
                setPixel(x + i, y + j, erased);
            }
        }
    }
};

class PressureBrush : public Brush {
public:
    void paint(int x, int y, Pixel color, int maxSize, int pressure, int angle, std::function<void(int, int, Pixel)> setPixel) override {
        (void)angle;
        float p = pressure / 255.0f;
        float opacityCurve = 0.2f + 0.8f * sqrt(p);
        if (opacityCurve > 1.0f) opacityCurve = 1.0f;
        float baseAlpha = (color.a * opacity / 255.0f) * opacityCurve;
        
        float effectiveDiameter = maxSize * (0.3f + 0.7f * p);
        float radius = effectiveDiameter / 2.0f; 
        if (radius < 0.5f) radius = 0.5f;
        int range = (int)ceil(radius) + 1;
        
        float featherRange = 1.5f;
        float maxDist = radius + (featherRange / 2.0f);
        float maxDist2 = maxDist * maxDist;

        for (int i = -range; i <= range; i++) {
            int i2 = i * i;
            for (int j = -range; j <= range; j++) {
                float dist2 = i2 + j*j;
                if (dist2 < maxDist2) {
                    float dist = sqrt(dist2);
                    float delta = (radius - dist + (featherRange / 2.0f)) / featherRange;
                    if (delta > 0.0f) {
                        if (delta > 1.0f) delta = 1.0f;
                        Pixel px = color;
                        px.a = (uint8_t)(baseAlpha * delta);
                        if (px.a > 0) setPixel(x + i, y + j, px);
                    }
                }
            }
        }
    }
};

class Airbrush : public Brush {
public:
    void paint(int x, int y, Pixel color, int size, int pressure, int angle, std::function<void(int, int, Pixel)> setPixel) override {
        (void)angle;
        float p = pressure / 255.0f;
        int effectiveSize = (int)(size * (0.5f + 0.5f * p)); 
        if (effectiveSize < 1) effectiveSize = 1;
        float pressureAlphaMod = 0.15f + 0.85f * p; 
        int r = effectiveSize; 
        int r2 = r * r;

        for (int i = -r; i <= r; i++) {
            int i2 = i * i;
            for (int j = -r; j <= r; j++) {
                int dist2 = i2 + j*j;
                if (dist2 <= r2) {
                    float dist = sqrt(dist2);
                    float falloff = 1.0f - (dist / r);
                    falloff = falloff * falloff; 
                    float finalAlphaFloat = (color.a * opacity / 255.0f) * pressureAlphaMod * falloff;
                    Pixel px = color;
                    px.a = (uint8_t)finalAlphaFloat;
                    if (px.a > 0) setPixel(x + i, y + j, px);
                }
            }
        }
    }
};

// --- NEW / MODIFIED BRUSHES ---

// 1. REAL PAINT BRUSH (Replaces the old textured brush)
// 4. TEXTURE BRUSH (Restored "Organic" Version)
class TexturedBrush : public Brush {
    // A smoother bristle map. Lower numbers are gaps between bristles.
    const float bristles[32] = { 
        0.3f, 0.7f, 0.9f, 0.5f, 0.2f, 0.8f, 0.9f, 0.4f, 
        0.9f, 0.6f, 0.3f, 0.8f, 0.9f, 0.2f, 0.7f, 0.5f,
        0.4f, 0.9f, 0.8f, 0.3f, 0.6f, 0.9f, 0.5f, 0.2f,
        0.8f, 0.4f, 0.9f, 0.7f, 0.3f, 0.8f, 0.6f, 0.4f
    };

public:
    void paint(int x, int y, Pixel color, int size, int pressure, int angle, std::function<void(int, int, Pixel)> setPixel) override {
        float rads = angle * (M_PI / 180.0f);
        float p = pressure / 255.0f;

        // Vector perpendicular to drawing direction
        float dx = -sin(rads);
        float dy = cos(rads);

        // Width doesn't change much with pressure to keep it controllable
        int halfWidth = size / 2;
        if (halfWidth < 1) halfWidth = 1;

        // Pressure Curve: High pressure rapidly approaches 1.0 (solid)
        // Low pressure stays near 0.0 (transparent/streaky)
        float pressurePower = pow(p, 0.5f); // Square root curve for fast ramp-up

        for (int i = -halfWidth; i <= halfWidth; i++) {
            int px = x + (int)(dx * i);
            int py = y + (int)(dy * i);

            // Sample bristle map. We mirror it for symmetry around the center.
            int patternIndex = abs(i) % 32; 
            float bristleStrength = bristles[patternIndex];

            // THE NEW MATH: Pressure fills in the gaps.
            // If pressure is high (1.0), combined strength becomes >= 1.0 everywhere.
            // If pressure is low, the bristle map dominates.
            float combinedStrength = bristleStrength + (pressurePower * 0.8f);
            if (combinedStrength > 1.0f) combinedStrength = 1.0f;
            
            // Soft Edges: A slight falloff at the very tips of the brush
            float edgeDist = (float)abs(i) / halfWidth;
            float edgeSoftness = 1.0f - pow(edgeDist, 4.0f); // Sharp falloff near edge

            float finalAlpha = (color.a / 255.0f) * (opacity / 255.0f) * combinedStrength * edgeSoftness;

            Pixel finalColor = color;
            finalColor.a = (uint8_t)(finalAlpha * 255);

            // Only draw visible pixels
            if (finalColor.a > 5) {
                setPixel(px, py, finalColor);
            }
        }
    }
};

// 2. NEW SOFT ERASER
class SoftEraserBrush : public Brush {
public:
    void paint(int x, int y, Pixel color, int size, int pressure, int angle, std::function<void(int, int, Pixel)> setPixel) override {
        (void)color; (void)angle;
        float p = pressure / 255.0f;
        
        // Dynamic size with pressure (like airbrush)
        int effectiveSize = (int)(size * (0.5f + 0.5f * p)); 
        if (effectiveSize < 1) effectiveSize = 1;
        
        // Pressure affects how STRONG the erasing is
        // 10% min strength -> 100% max
        float pressureMod = 0.1f + 0.9f * p; 
        
        int r = effectiveSize; 
        int r2 = r * r;

        for (int i = -r; i <= r; i++) {
            int i2 = i * i;
            for (int j = -r; j <= r; j++) {
                int dist2 = i2 + j*j;
                if (dist2 <= r2) {
                    float dist = sqrt(dist2);
                    // Soft edges (Cubic falloff)
                    float falloff = 1.0f - (dist / r);
                    falloff = falloff * falloff * falloff;
                    
                    // Calculate "Eraser Strength" (0 to 255)
                    // We store this in the ALPHA channel of the pixel we pass
                    uint8_t strength = (uint8_t)(255 * falloff * pressureMod * (opacity / 255.0f));
                    
                    if (strength > 0) {
                        // We send {0,0,0, strength}. 
                        // The RGB doesn't matter, only the Alpha (Strength).
                        Pixel p = {0, 0, 0, strength};
                        setPixel(x + i, y + j, p);
                    }
                }
            }
        }
    }
};

#endif
