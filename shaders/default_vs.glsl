#version 450
#pragma shader_stage(vertex)

layout(location = 0) in vec3 iPosition;
layout(location = 1) in vec3 iColor;

layout(location = 0) out vec3 aColor;

layout(binding = 0) uniform transform {
    mat4 u_model;
    mat4 u_view;
    mat4 u_proj;
};

void main()
{
    gl_Position = u_proj * u_view * u_model * vec4(iPosition, 1.0);
    aColor = iColor;
}
