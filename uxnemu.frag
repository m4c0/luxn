#version 450

layout(set = 0, binding = 0) uniform sampler2D text;

layout(location = 0) in vec2 frag_coord;

layout(location = 0) out vec4 frag_colour;

void main() {
  frag_colour = texture(text, frag_coord);
}
