#version 450
#pragma shader_stage(fragment)

layout(location = 0) in vec3 aColor;
layout(location = 1) in vec2 aUv;

layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D uTexSampler;

void main()
{
    fragColor = vec4(aColor, 1.0) * texture(uTexSampler, aUv);
}
