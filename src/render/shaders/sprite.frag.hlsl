// Muestra el atlas y multiplica por el color de la instancia. Enlace de SDL_GPU para
// fragmento: textura y sampler en (t0, s0, space2) = set 2, binding 0 en SPIR-V, donde
// SDL espera un sampler combinado.

[[vk::combinedImageSampler]] [[vk::binding(0, 2)]]
Texture2D<float4> atlas : register(t0, space2);
[[vk::combinedImageSampler]] [[vk::binding(0, 2)]]
SamplerState atlas_sampler : register(s0, space2);

struct Input
{
    [[vk::location(0)]] float2 uv : TEXCOORD0;
    [[vk::location(1)]] float4 color : TEXCOORD1;
};

float4 main(Input input) : SV_Target0
{
    return atlas.Sample(atlas_sampler, input.uv) * input.color;
}
