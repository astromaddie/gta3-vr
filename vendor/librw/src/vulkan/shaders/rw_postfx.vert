#version 450

// A single oversized triangle covers the complete render target without a
// vertex buffer or a diagonal seam.
void main()
{
	vec2 position;
	if(gl_VertexIndex == 0)
		position = vec2(-1.0, -1.0);
	else if(gl_VertexIndex == 1)
		position = vec2(-1.0, 3.0);
	else
		position = vec2(3.0, -1.0);
	gl_Position = vec4(position, 0.0, 1.0);
}
