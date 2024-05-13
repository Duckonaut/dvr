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

    if (length(tex_coords - push_constants.emitter_pos) < push_constants.emitter_radius) {
        imageStore(out_img, tex_coords, vec4(1.0, 1.0, 1.0, 1.0));
    }
}
