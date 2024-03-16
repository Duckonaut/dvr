#version 450
#pragma shader_stage(fragment)

layout(location = 0) in vec3 aColor;

layout(location = 0) out vec4 fragColor;

void main()
{
    fragColor = vec4(aColor, 1.0);
}
