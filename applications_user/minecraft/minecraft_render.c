#include "minecraft_app.h"
#include <gui/canvas.h>
#include <math.h>

#define SCREEN_W 240
#define SCREEN_H 135
#define FOCAL_LENGTH 120.0f

typedef struct {
    float x, y, z;
} Point3D;

typedef struct {
    int x, y;
} Point2D;

static Point2D project(Point3D p, float yaw, float pitch) {
    // Basic rotation around Y (yaw)
    float cosY = cosf(yaw);
    float sinY = sinf(yaw);
    float x = p.x * cosY - p.z * sinY;
    float z = p.x * sinY + p.z * cosY;
    
    // Basic rotation around X (pitch)
    float cosX = cosf(pitch);
    float sinX = sinf(pitch);
    float y = p.y * cosX - z * sinX;
    z = p.y * sinX + z * cosX;

    Point2D p2;
    if (z <= 0.1f) {
        p2.x = -1;
        p2.y = -1;
        return p2;
    }

    p2.x = (int)(x * FOCAL_LENGTH / z) + (SCREEN_W / 2);
    p2.y = (int)(y * FOCAL_LENGTH / z) + (SCREEN_H / 2);
    return p2;
}

void render_cube(Canvas* canvas, float px, float py, float pz, float yaw, float pitch, float size) {
    Point3D vertices[8] = {
        {px - size, py - size, pz - size}, {px + size, py - size, pz - size},
        {px + size, py + size, pz - size}, {px - size, py + size, pz - size},
        {px - size, py - size, pz + size}, {px + size, py - size, pz + size},
        {px + size, py + size, pz + size}, {px - size, py + size, pz + size}
    };

    Point2D projected[8];
    for (int i = 0; i < 8; i++) {
        projected[i] = project(vertices[i], yaw, pitch);
    }

    canvas_set_color(canvas, ColorBlack);
    
    // Draw edges
    for (int i = 0; i < 4; i++) {
        int next = (i + 1) % 4;
        if (projected[i].x != -1 && projected[next].x != -1)
            canvas_draw_line(canvas, projected[i].x, projected[i].y, projected[next].x, projected[next].y);
        if (projected[i+4].x != -1 && projected[next+4].x != -1)
            canvas_draw_line(canvas, projected[i+4].x, projected[i+4].y, projected[next+4].x, projected[next+4].y);
        if (projected[i].x != -1 && projected[i+4].x != -1)
            canvas_draw_line(canvas, projected[i].x, projected[i].y, projected[i+4].x, projected[i+4].y);
    }
}
