#version 300 es

precision highp float;
in vec2 v_texcoord;

uniform sampler2D tex;          // window surface texture
uniform sampler2D blurredBG;    // previous ambient frame (ping-pong)
uniform vec2 topLeft;           // window top-left in UV coords (0-1)
uniform vec2 bottomRight;       // window bottom-right in UV coords (0-1)
uniform vec2 fullSize;          // monitor size in pixels
uniform float alpha;            // overall alpha

layout(location = 0) out vec4 fragColor;

// Average a broad area of the window texture near a given edge point.
// This destroys any recognizable features, leaving only the dominant color.
vec3 sampleEdgeColor(vec2 edgeUV, vec2 winSize) {
    // Sample a grid across a large area near the edge, in window-texture UV space
    vec2 sampleSpread = 80.0 / winSize; // 80px spread in window coords
    vec3 col = vec3(0.0);
    col += texture(tex, edgeUV).rgb;
    col += texture(tex, edgeUV + vec2( sampleSpread.x, 0.0)).rgb;
    col += texture(tex, edgeUV + vec2(-sampleSpread.x, 0.0)).rgb;
    col += texture(tex, edgeUV + vec2(0.0,  sampleSpread.y)).rgb;
    col += texture(tex, edgeUV + vec2(0.0, -sampleSpread.y)).rgb;
    col += texture(tex, edgeUV + vec2( sampleSpread.x,  sampleSpread.y)).rgb;
    col += texture(tex, edgeUV + vec2(-sampleSpread.x,  sampleSpread.y)).rgb;
    col += texture(tex, edgeUV + vec2( sampleSpread.x, -sampleSpread.y)).rgb;
    col += texture(tex, edgeUV + vec2(-sampleSpread.x, -sampleSpread.y)).rgb;

    vec2 wide = sampleSpread * 2.5;
    col += texture(tex, edgeUV + vec2( wide.x, 0.0)).rgb;
    col += texture(tex, edgeUV + vec2(-wide.x, 0.0)).rgb;
    col += texture(tex, edgeUV + vec2(0.0,  wide.y)).rgb;
    col += texture(tex, edgeUV + vec2(0.0, -wide.y)).rgb;

    return col / 13.0;
}

void main() {
    vec2 uv = v_texcoord;
    vec2 texel = 1.0 / fullSize;

    // Window dimensions
    vec2 winUVSize = bottomRight - topLeft;
    vec2 winPixelSize = winUVSize * fullSize;

    // Compute nearest point on window rectangle (in UV space)
    vec2 nearest = clamp(uv, topLeft, bottomRight);
    float dist = length((uv - nearest) * fullSize); // distance in pixels

    // Are we inside the window?
    bool inside = (uv.x >= topLeft.x && uv.x <= bottomRight.x &&
                   uv.y >= topLeft.y && uv.y <= bottomRight.y);

    // Edge strip: a band just inside the window border that seeds color
    float edgeDist = 0.0;
    if (inside) {
        float dLeft   = (uv.x - topLeft.x) * fullSize.x;
        float dRight  = (bottomRight.x - uv.x) * fullSize.x;
        float dTop    = (uv.y - topLeft.y) * fullSize.y;
        float dBottom = (bottomRight.y - uv.y) * fullSize.y;
        edgeDist = min(min(dLeft, dRight), min(dTop, dBottom));
    }

    float stripWidth = 60.0; // pixels
    bool isEdgeStrip = inside && edgeDist < stripWidth;

    if (inside && !isEdgeStrip) {
        // Deep inside window: black (actual window renders on top)
        fragColor = vec4(0.0, 0.0, 0.0, alpha);
        return;
    }

    if (isEdgeStrip) {
        // Map to window texture UV, clamped inward to avoid edge artifacts
        vec2 winUV = (uv - topLeft) / winUVSize;
        vec2 margin = 2.0 / winPixelSize; // 2px inward margin
        winUV = clamp(winUV, margin, 1.0 - margin);

        // Sample a broad blurred area, not a single pixel
        vec3 edgeColor = sampleEdgeColor(winUV, winPixelSize);

        // Suppress near-black to prevent ghostly white bleed on dark content
        float brightness = dot(edgeColor, vec3(0.299, 0.587, 0.114));
        edgeColor *= smoothstep(0.01, 0.05, brightness);

        // Blend with previous frame: gentle seeding, stronger near the edge
        vec3 prev = texture(blurredBG, uv).rgb;
        float seedWeight = 0.12 * (1.0 - edgeDist / stripWidth);
        fragColor = vec4(mix(prev, edgeColor, seedWeight), alpha);
        return;
    }

    // --- Outside the window: diffuse from previous frame neighbors ---
    // Aggressive multi-scale sampling to destroy recognizable features.
    // Large step sizes create smooth color gradients, not shifted copies.
    vec3 sum = vec3(0.0);
    float totalWeight = 0.0;

    // Scale 1: fine (~8px) — local smoothing
    float r1 = 8.0;
    float w1 = 1.0;
    sum += texture(blurredBG, uv + vec2(texel.x * r1, 0.0)).rgb * w1;
    sum += texture(blurredBG, uv - vec2(texel.x * r1, 0.0)).rgb * w1;
    sum += texture(blurredBG, uv + vec2(0.0, texel.y * r1)).rgb * w1;
    sum += texture(blurredBG, uv - vec2(0.0, texel.y * r1)).rgb * w1;
    sum += texture(blurredBG, uv + vec2(texel.x * r1, texel.y * r1) * 0.707).rgb * w1;
    sum += texture(blurredBG, uv - vec2(texel.x * r1, texel.y * r1) * 0.707).rgb * w1;
    sum += texture(blurredBG, uv + vec2(texel.x * r1, -texel.y * r1) * 0.707).rgb * w1;
    sum += texture(blurredBG, uv - vec2(texel.x * r1, -texel.y * r1) * 0.707).rgb * w1;
    totalWeight += w1 * 8.0;

    // Scale 2: medium (~40px) — spread color across the bar
    float r2 = 40.0;
    float w2 = 0.8;
    sum += texture(blurredBG, uv + vec2(texel.x * r2, 0.0)).rgb * w2;
    sum += texture(blurredBG, uv - vec2(texel.x * r2, 0.0)).rgb * w2;
    sum += texture(blurredBG, uv + vec2(0.0, texel.y * r2)).rgb * w2;
    sum += texture(blurredBG, uv - vec2(0.0, texel.y * r2)).rgb * w2;
    sum += texture(blurredBG, uv + vec2(texel.x * r2, texel.y * r2) * 0.707).rgb * w2;
    sum += texture(blurredBG, uv - vec2(texel.x * r2, texel.y * r2) * 0.707).rgb * w2;
    sum += texture(blurredBG, uv + vec2(texel.x * r2, -texel.y * r2) * 0.707).rgb * w2;
    sum += texture(blurredBG, uv - vec2(texel.x * r2, -texel.y * r2) * 0.707).rgb * w2;
    totalWeight += w2 * 8.0;

    // Scale 3: large (~120px) — long-range color transport
    float r3 = 120.0;
    float w3 = 0.5;
    sum += texture(blurredBG, uv + vec2(texel.x * r3, 0.0)).rgb * w3;
    sum += texture(blurredBG, uv - vec2(texel.x * r3, 0.0)).rgb * w3;
    sum += texture(blurredBG, uv + vec2(0.0, texel.y * r3)).rgb * w3;
    sum += texture(blurredBG, uv - vec2(0.0, texel.y * r3)).rgb * w3;
    sum += texture(blurredBG, uv + vec2(texel.x * r3, texel.y * r3) * 0.707).rgb * w3;
    sum += texture(blurredBG, uv - vec2(texel.x * r3, texel.y * r3) * 0.707).rgb * w3;
    totalWeight += w3 * 6.0;

    vec3 diffused = sum / totalWeight;

    // Distance-based falloff: color fades to black with distance from window.
    // Use the window's shorter dimension as the spread reference so that
    // the effect scales with window size, not monitor size.
    // On a 32:9 monitor with a 16:9 window this gives generous spread.
    float spreadDist = min(winPixelSize.x, winPixelSize.y) * 0.6;
    // Smooth cubic falloff: stays bright near the window, gentle taper
    float t = clamp(dist / spreadDist, 0.0, 1.0);
    float distanceFade = 1.0 - t * t * (3.0 - 2.0 * t); // smootherstep

    // Per-frame decay: very gentle so colors persist and fill the space
    diffused *= 0.998 * distanceFade;

    // Suppress near-black noise floor
    float lum = dot(diffused, vec3(0.299, 0.587, 0.114));
    diffused *= smoothstep(0.002, 0.01, lum);

    fragColor = vec4(diffused, alpha);
}
