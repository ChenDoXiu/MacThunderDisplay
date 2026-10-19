#version 450
layout(binding = 0) uniform sampler2D yp;
layout(binding = 1) uniform sampler2D up;
layout(binding = 2) uniform sampler2D vp;
layout(binding = 3) uniform sampler2D cursorImage;
layout(binding = 4) uniform sampler2D statsImage;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outputColor;

layout(push_constant) uniform Constants {
    int srgbAttachment;
    int cursorVisible;
    float cursorLeft;
    float cursorTop;
    float cursorWidth;
    float cursorHeight;
    int statsVisible;
    float statsWidth;
    float statsHeight;
    float statsLeft;
    float statsTop;
}

pc;

vec3 toRgb(float y, vec2 p) {
    float u = (textureLod(up, p, 0.0).r * 255.0 - 128.0) / 224.0;
    float v = (textureLod(vp, p, 0.0).r * 255.0 - 128.0) / 224.0;
    return clamp(vec3(y + 1.5748 * v, y - 0.187324 * u - 0.468124 * v, y + 1.8556 * u), 0.0, 1.0);
}

void main() {
    float y = (textureLod(yp, uv, 0.0).r * 255.0 - 16.0) / 219.0;
    vec3 rgb = toRgb(y, uv);
    if (pc.cursorVisible != 0) {
        vec2 local =
            (uv - vec2(pc.cursorLeft, pc.cursorTop)) / vec2(pc.cursorWidth, pc.cursorHeight);
        if (all(greaterThanEqual(local, vec2(0))) && all(lessThan(local, vec2(1)))) {
            vec4 cursor = texture(cursorImage, local);
            rgb = cursor.rgb + rgb * (1.0 - cursor.a);
        }
    }
    if (pc.statsVisible != 0) {
        vec2 local = (gl_FragCoord.xy - vec2(pc.statsLeft, pc.statsTop)) /
                     vec2(pc.statsWidth, pc.statsHeight);
        if (all(greaterThanEqual(local, vec2(0))) && all(lessThan(local, vec2(1)))) {
            vec4 stats = texture(statsImage, local);
            rgb = stats.rgb + rgb * (1.0 - stats.a);
        }
    }
    // YUV already represents nonlinear RGB. Avoid encoding it twice when the
    // surface only offers an sRGB attachment instead of the preferred UNORM.
    if (pc.srgbAttachment != 0)
        rgb = mix(pow((rgb + 0.055) / 1.055, vec3(2.4)), rgb / 12.92,
                  lessThanEqual(rgb, vec3(0.04045)));
    outputColor = vec4(rgb, 1.0);
}
