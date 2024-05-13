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
    ivec2 world_size;
    uint num_particles;
    float dt;
    float speed;
    float turn_speed;
    float random_steer;
    float sensor_angle;
    float sensor_dist;
} push_constants;

float randf(vec2 seed) {
    // random number generator
    return fract(sin(dot(seed, vec2(12.9898, 78.233))) * 43758.5453);
}

float sniff_area(vec2 pos, float angle, float dt, float sniff_angle, float sniff_dist, int sniff_area_size) {
    vec2 base_pos = pos + vec2(cos(angle + sniff_angle), sin(angle + sniff_angle)) * sniff_dist;

    float sum = 0.0;
    ivec2 base_pos_i = ivec2(base_pos);

    for (int i = -sniff_area_size; i <= sniff_area_size; i++) {
        for (int j = -sniff_area_size; j <= sniff_area_size; j++) {
            ivec2 pos_i = base_pos_i + ivec2(i, j);
            pos_i.x = pos_i.x % push_constants.world_size.x;
            pos_i.y = pos_i.y % push_constants.world_size.y;
            vec2 pos_f = vec2(pos_i);

            float val = imageLoad(in_img, pos_i).r;
            sum += val;
        }
    }

    return sum;
}

layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;
void main()
{
    uint index = gl_GlobalInvocationID.x;
    if (index >= push_constants.num_particles) {
        return;
    }

    float dt = push_constants.dt;

    particle p = in_particles[index];
    vec2 pos = p.pos;
    float angle = p.angle;

    float sensor_angle = push_constants.sensor_angle;
    float sensor_dist = push_constants.sensor_dist;
    int sensor_area_size = 2;

    float left = sniff_area(pos, angle, push_constants.dt, sensor_angle, sensor_dist, sensor_area_size);
    float right = sniff_area(pos, angle, push_constants.dt, -sensor_angle, sensor_dist, sensor_area_size);
    float forward = sniff_area(pos, angle, push_constants.dt, 0.0, sensor_dist, sensor_area_size);

    float random_steer = (randf(pos) - 0.5) * push_constants.random_steer * 2.0;

    if (forward > left && forward > right) {
        angle += random_steer * dt * 0.25;
    } else if (forward < left && forward < right) {
        angle += random_steer * dt;
    }
    else if (left > right) {
        angle += sensor_angle * dt * push_constants.turn_speed + random_steer * dt * 0.5;
    } else {
        angle -= sensor_angle * dt * push_constants.turn_speed + random_steer * dt * 0.5;
    }

    vec2 vel = vec2(cos(angle), sin(angle));
    vec2 new_pos = pos + vel * push_constants.dt * push_constants.speed;

    new_pos.x = mod(new_pos.x, float(push_constants.world_size.x));
    new_pos.y = mod(new_pos.y, float(push_constants.world_size.y));

    out_particles[index].pos = new_pos;
    out_particles[index].angle = angle;

    imageStore(out_img, ivec2(new_pos), vec4(1.0, 0.0, 0.0, 0.0));
}
