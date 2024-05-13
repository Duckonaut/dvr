#version 450
#pragma shader_stage(fragment)

layout(location = 0) in vec2 aUv;

layout(location = 0) out vec4 fragColor;

layout(binding = 0) uniform sampler2D uTexSampler;

// a nice gradient
vec4 beautiful_gradient(float t)
{
    return vec4(t * t, t * t * t, t, 1.0);
}

void main()
{
    float value = texture(uTexSampler, aUv).r;
    fragColor = beautiful_gradient(value);
}
