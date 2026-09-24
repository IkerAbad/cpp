// Quads instanciados: cada instancia es un sprite; los 6 vértices del quad salen de
// SV_VertexID, sin búfer de vértices. Convenciones de enlace de SDL_GPU (SDL_gpu.h,
// SDL_CreateGPUShader): uniforms del vértice en (b0, space1) = set 1 en SPIR-V;
// atributos con semántica TEXCOORD0..N y la misma location en SPIR-V.

cbuffer ViewUniforms : register(b0, space1)
{
    float2 screen_size;  // píxeles de la ventana
    float2 unused;
};

struct Input
{
    [[vk::location(0)]] float2 pos : TEXCOORD0;    // esquina superior izquierda, píxeles
    [[vk::location(1)]] float2 size : TEXCOORD1;   // píxeles
    [[vk::location(2)]] float4 uv : TEXCOORD2;     // u0, v0, u1, v1 normalizados
    [[vk::location(3)]] float4 color : TEXCOORD3;  // RGBA normalizado, multiplica al texel
    uint vertex_id : SV_VertexID;
};

struct Output
{
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv : TEXCOORD0;
    [[vk::location(1)]] float4 color : TEXCOORD1;
};

static const float2 kCorners[6] = {
    float2(0.0, 0.0), float2(1.0, 0.0), float2(0.0, 1.0),
    float2(0.0, 1.0), float2(1.0, 0.0), float2(1.0, 1.0),
};

Output main(Input input)
{
    const float2 k = kCorners[input.vertex_id];
    const float2 p = input.pos + input.size * k;
    Output output;
    // SDL_GPU usa NDC con +Y hacia arriba en todos los backends (convierte en Vulkan).
    output.position = float4(p.x / screen_size.x * 2.0 - 1.0, 1.0 - p.y / screen_size.y * 2.0, 0.0, 1.0);
    output.uv = lerp(input.uv.xy, input.uv.zw, k);
    output.color = input.color;
    return output;
}
