#pragma once

#include <NF/Core/Types.hpp>
#include <NF/Core/Math.hpp> // for Vec3, Mat4 if available, otherwise we define minimal

namespace nf::rendering {

// Minimal math for Camera if Core Math not sufficient — we use simple structs
struct Vec3 {
    float x=0,y=0,z=0;
    Vec3() = default;
    Vec3(float _x,float _y,float _z):x(_x),y(_y),z(_z){}
};

struct Mat4 {
    float m[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    static Mat4 identity();
    static Mat4 perspective(float fov_y_rad, float aspect, float near_plane, float far_plane);
    static Mat4 orthographic(float left, float right, float bottom, float top, float near_plane, float far_plane);
    static Mat4 look_at(const Vec3& eye, const Vec3& center, const Vec3& up);
    static Mat4 translation(float x, float y, float z);
    Mat4 operator*(const Mat4& other) const;
    /// Full general 4x4 inverse (cofactor/determinant). Used for invViewProj —
    /// the deferred lighting pass reconstructs world position from depth.
    Mat4 inverse() const;
};

struct Plane {
    float a=0,b=0,c=0,d=0; // ax+by+cz+d=0, normalized
    float distance(float x,float y,float z) const { return a*x + b*y + c*z + d; }
};

struct Frustum {
    Plane planes[6]; // left, right, bottom, top, near, far
    bool contains_sphere(float x,float y,float z,float radius) const;
    bool contains_aabb(float min_x,float min_y,float min_z,float max_x,float max_y,float max_z) const;
};

// Camera component — lives in ECS World
struct Camera {
    enum class ProjectionType { Perspective, Orthographic };

    ProjectionType type = ProjectionType::Perspective;
    float fov_y_rad = 60.0f * 3.14159265359f / 180.0f;
    float aspect = 16.0f/9.0f;
    float near_plane = 0.1f;
    float far_plane = 1000.0f;

    // Ortho params
    float ortho_width = 10.0f;
    float ortho_height = 10.0f;

    // Cached matrices (updated via update_camera)
    Mat4 view;
    Mat4 projection;
    Mat4 view_projection;
    Frustum frustum;

    Vec3 position{0,0,5};
    Vec3 target{0,0,0};
    Vec3 up{0,1,0};
};

void update_camera(Camera& cam);
Frustum extract_frustum(const Mat4& view_proj);

} // namespace nf::rendering
