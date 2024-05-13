#version 450
#pragma shader_stage(fragment)

layout(location = 0) in vec2 aUv;

layout(location = 0) out vec4 fragColor;

layout(binding = 0) uniform sampler2D uTexSampler;

layout(push_constant) uniform PushConstants
{
    float hue;
} pc;

// h in range [0, 360], s in range [0, 1], l in range [0, 1]
vec3 hsl_to_rgb(vec3 hsl)
{
    float c = (1.0 - abs(2.0 * hsl.z - 1.0)) * hsl.y;
    float x = c * (1.0 - abs(mod(hsl.x / 60.0, 2.0) - 1.0));
    float m = hsl.z - c / 2.0;

    vec3 rgb;
    if (hsl.x < 60.0)
    {
        rgb = vec3(c, x, 0.0);
    }
    else if (hsl.x < 120.0)
    {
        rgb = vec3(x, c, 0.0);
    }
    else if (hsl.x < 180.0)
    {
        rgb = vec3(0.0, c, x);
    }
    else if (hsl.x < 240.0)
    {
        rgb = vec3(0.0, x, c);
    }
    else if (hsl.x < 300.0)
    {
        rgb = vec3(x, 0.0, c);
    }
    else
    {
        rgb = vec3(c, 0.0, x);
    }

    return rgb + vec3(m);
}

// a nice gradient from black to white with the given hue in the middle
vec4 beautiful_gradient(float t)
{
    float hue = pc.hue;
    float s = 1.0 - t * t * t;
    float l = t;

    return vec4(hsl_to_rgb(vec3(hue, s, l)), 1.0);
}

void main()
{
    float value = texture(uTexSampler, aUv).z;
    fragColor = beautiful_gradient(value);
}

