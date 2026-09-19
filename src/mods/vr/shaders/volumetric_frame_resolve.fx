// Resolve one active rendered eye into its conservative baseline crop.
// Both SRV and RTV are sRGB: filter in linear light and encode once on output.
cbuffer Resolve : register(b0) {
    float4 output_rect;
    float4 source_rect;
};
Texture2D<float4> scene : register(t0);
SamplerState linear_clamp : register(s0);

float4 ResolvePS(float4 position : SV_Position) : SV_Target {
    if (all(source_rect.zw == output_rect.zw)) {
        // Reduced views map one source pixel to one output pixel. Avoid
        // normalized-coordinate rounding introducing unwanted filtering.
        return scene.Load(int3(int2(source_rect.xy) + int2(position.xy - output_rect.xy), 0));
    }
    uint width, height;
    scene.GetDimensions(width, height);
    float2 fraction = (position.xy - output_rect.xy) / output_rect.zw;
    float2 texel = source_rect.xy + fraction * source_rect.zw;
    // Clamp within the active eye, so filtering cannot bleed across the seam.
    texel = clamp(texel, source_rect.xy + 0.5, source_rect.xy + source_rect.zw - 0.5);
    return scene.SampleLevel(linear_clamp, texel / float2(width, height), 0);
}
