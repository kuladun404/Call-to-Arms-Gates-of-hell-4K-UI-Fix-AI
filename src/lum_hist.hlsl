// Копия шейдера игры (автоэкспозиция, шаг 2: гистограмма яркости, 1024 корзины).
// Отличие одно: пиксель берётся равномерно по всей картинке мира (src_px), а не по номеру потока.
// histoData.xy — размер, который знает игра (логический).
Texture2D<float4> scene_Buffer : register(t0);
RWTexture2D<uint> histogramBins_Buffer : register(u0);
RWTexture2D<uint> histogramYRange_Buffer : register(u1);
RWTexture2D<float> histogramMetaData_Buffer : register(u3);
cbuffer hist_params : register(b0) { float4 histoData; float4 histoData2; };

groupshared float g_scale;

uint2 src_px(uint2 id, uint2 sz)
{
    uint w, h;
    scene_Buffer.GetDimensions(w, h);
    return min((uint2)(((float2)id + 0.5) * float2(w, h) / (float2)sz), uint2(w - 1, h - 1));
}

[numthreads(32, 32, 1)]
void main(uint3 tig : SV_GroupThreadID, uint3 id : SV_DispatchThreadID)
{
    uint2 sz = (uint2)histoData.xy;
    if (tig.x == 0 && tig.y == 0) g_scale = 1024.0 / histogramMetaData_Buffer[uint2(0, 0)];
    GroupMemoryBarrierWithGroupSync();
    if (id.x < sz.x && id.y < sz.y) {
        float3 c = scene_Buffer.Load(int3(src_px(id.xy, sz), 0)).rgb;
        float l = dot(c, float3(0.2125, 0.7154, 0.0721));
        uint b = (uint)min(round(l * g_scale), 1023.0);
        uint2 bc = uint2(b & 1023, b >> 10);
        InterlockedAdd(histogramBins_Buffer[bc], 1);
        if (0 < histoData.z) {
            uint v = histogramBins_Buffer[bc];
            InterlockedMax(histogramYRange_Buffer[uint2(0, 0)], v);
        }
    }
}
