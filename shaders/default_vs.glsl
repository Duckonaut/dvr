#version 450
#pragma shader_stage(vertex)

layout(location = 0) in vec3 iPosition;
layout(location = 1) in vec3 iColor;
layout(location = 2) in vec3 instPosition;

layout(location = 0) out vec3 aColor;

layout(binding = 0) uniform transform {
    mat4 u_model;
    mat4 u_view;
    mat4 u_proj;
};

void main()
{
    mat4 inst_model = u_model;
    inst_model[3][0] += instPosition.x;
    inst_model[3][1] += instPosition.y;
    inst_model[3][2] += instPosition.z;
    gl_Position = u_proj * u_view * inst_model * vec4(iPosition, 1.0);
    aColor = iColor;
}
