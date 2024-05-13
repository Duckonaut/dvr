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

vec3 diffuse(ivec2 pos, ivec2 tex_size, float dt, float diffuse_strength)
{
    vec3 start_density = imageLoad(in_img, pos).xyz;
    float a = dt * diffuse_strength;

    vec3 new_density = start_density;

    for (int i = -1; i <= 1; i++) {
        for (int j = -1; j <= 1; j++) {
            if (i == 0 && j == 0) {
                continue;
            }
            ivec2 neighbour_pos = pos + ivec2(i, j);
            neighbour_pos.x = (neighbour_pos.x + tex_size.x) % tex_size.x;
            neighbour_pos.y = (neighbour_pos.y + tex_size.y) % tex_size.y;

            vec3 neighbour_density = imageLoad(in_img, neighbour_pos).xyz;
            new_density += a * neighbour_density;
        }
    }

    return new_density / (1.0 + 9.0 * a);
}

layout(local_size_x = 32, local_size_y = 32, local_size_z = 1) in;
void main()
{
    ivec2 tex_size = imageSize(in_img);
    ivec2 tex_coords = ivec2(gl_GlobalInvocationID.xy);

    if (tex_coords.x >= tex_size.x || tex_coords.y >= tex_size.y) {
        return;
    }

    vec4 data = imageLoad(in_img, tex_coords);
    vec3 diffused = diffuse(tex_coords, tex_size, push_constants.dt, push_constants.diffuse_strength);
    diffused -= 0.1 * push_constants.dt;

    imageStore(out_img, tex_coords, vec4(diffused, data.a));
}
