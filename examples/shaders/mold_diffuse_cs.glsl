#version 450
#pragma shader_stage(compute)

struct particle {
    vec2 pos;
    float angle;
};

layout(std140, binding = 0) readonly buffer in_particle_buf {
    particle in_particles[];
};

layout(std140, binding = 1) buffer out_particle_buf {
    particle out_particles[];
};

layout(binding = 2, r32f) uniform readonly image2D in_img;
layout(binding = 3, r32f) uniform writeonly image2D out_img;

layout(push_constant) uniform PushConstants {
    float dt;
    float blur_strength;
    float decay;
} push_constants;

float blur(ivec2 pos, ivec2 tex_size, float dt, float blur_strength)
{
    float new_color = 0.0;
    float weight_sum = 0.0;

    for (int x = -1; x <= 1; x++) {
        for (int y = -1; y <= 1; y++) {
            ivec2 offset = ivec2(x, y);
            ivec2 new_pos = pos + offset;
            new_pos.x = (new_pos.x + tex_size.x) % tex_size.x;
            new_pos.y = (new_pos.y + tex_size.y) % tex_size.y;

            float weight = 1.0;
            if (x != 0 || y != 0) {
                weight = blur_strength;
            }
            new_color += imageLoad(in_img, new_pos).r * weight;
            weight_sum += weight;
        }
    }

    new_color /= weight_sum;
    return new_color;
}

layout(local_size_x = 32, local_size_y = 32, local_size_z = 1) in;
void main()
{
    ivec2 tex_size = imageSize(in_img);
    ivec2 tex_coords = ivec2(gl_GlobalInvocationID.xy);

    if (tex_coords.x >= tex_size.x || tex_coords.y >= tex_size.y) {
        return;
    }

    float new_color = blur(tex_coords, tex_size, push_constants.dt, push_constants.blur_strength);
    new_color -= push_constants.decay * new_color;
    if (new_color < 0.0) {
        new_color = 0.0;
    }

    imageStore(out_img, tex_coords, vec4(new_color, 0.0, 0.0, 0.0));
}

