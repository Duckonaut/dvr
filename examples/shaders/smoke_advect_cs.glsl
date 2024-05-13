#version 450
#pragma shader_stage(compute)

layout(binding = 0, rgba8) uniform readonly image2D in_img;
layout(binding = 1, rgba8) uniform writeonly image2D out_img;

layout(push_constant) uniform PushConstants {
    float dt;
    float diffuse_strength;
    vec2 emitter_pos;
    float emitter_radius;
} push_constants;

layout(local_size_x = 32, local_size_y = 32, local_size_z = 1) in;
void main()
{
    ivec2 tex_size = imageSize(in_img);
    ivec2 tex_coords = ivec2(gl_GlobalInvocationID.xy);

    if (tex_coords.x >= tex_size.x || tex_coords.y >= tex_size.y) {
        return;
    }

    vec4 data = imageLoad(in_img, tex_coords);
    float density = data.z;

    // advect smoke
    float dt = push_constants.dt;
    vec2 pos = vec2(tex_coords) - dt * data.xy;
    pos = clamp(pos, vec2(0), vec2(tex_size - 1));
    ivec2 pos0 = ivec2(pos);
    ivec2 pos1 = ivec2(pos + 1.0);
    float s1 = pos.x - pos0.x;
    float s0 = 1.0 - s1;
    float t1 = pos.y - pos0.y;
    float t0 = 1.0 - t1;

    float value =
        s0 * (t0 * imageLoad(in_img, pos0).z + t1 * imageLoad(in_img, ivec2(pos0.x, pos1.y)).z) +
        s1 * (t0 * imageLoad(in_img, ivec2(pos1.x, pos0.y)).z + t1 * imageLoad(in_img, pos1).z);

    imageStore(out_img, tex_coords, vec4(data.xy, value, 1.0));
}

