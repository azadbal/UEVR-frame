// Draw only outside the portal; discarded pixels retain the game's stereo image.
cbuffer Frame : register(b0) {
    float4 eye_x;
    float4 eye_y;
    float4 eye_z;
    float4 eye_origin;
    float4 fov_tangents; // left, right, up, down
    float4 viewport; // x, y, width, height of the submitted eye sub-image
    float4 half_size;
    float4 background;
};

float4 FrameVS(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 FramePS(float4 position : SV_Position) : SV_Target {
    float2 uv = (position.xy - viewport.xy) / viewport.zw;
    float3 ray = float3(lerp(fov_tangents.x, fov_tangents.y, uv.x),
                       lerp(fov_tangents.z, fov_tangents.w, uv.y), -1);
    float3 direction = eye_x.xyz * ray.x + eye_y.xyz * ray.y + eye_z.xyz * ray.z;

    // A one-sided window: crossing its plane or looking away hides the game.
    if (eye_origin.z > 0 && direction.z < -0.000001) {
        float t = -eye_origin.z / direction.z;
        float2 hit = eye_origin.xy + direction.xy * t;
        if (all(abs(hit) <= half_size.xy)) {
            discard;
        }
    }
    return background;
}
