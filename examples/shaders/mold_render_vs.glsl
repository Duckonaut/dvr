#version 450
#pragma shader_stage(vertex)

const vec2 positions[3] = vec2[3](
    vec2(-1.0, -1.0),
    vec2(3.0,  -1.0),
    vec2(-1.0,  3.0)
);

const vec2 uvs[3] = vec2[3](
    vec2(0.0, 0.0),
    vec2(2.0, 0.0),
    vec2(0.0, 2.0)
);

layout(location = 0) out vec2 outUV;

void main()
{
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    outUV = uvs[gl_VertexIndex];
}

