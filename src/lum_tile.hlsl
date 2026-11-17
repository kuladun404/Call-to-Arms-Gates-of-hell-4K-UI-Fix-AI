// Копия шейдера игры (автоэкспозиция, шаг 1: средняя и наибольшая яркость каждой плитки 32x32).
// Отличие одно: пиксель берётся не по номеру потока, а равномерно по всей картинке мира (src_px).
// histoData.zw — размер, который знает игра (логический); картинка мира может быть больше/меньше (F14, масштаб).
Texture2D<float4> scene_Buffer : register(t0);
RWTexture2D<uint> histogramMetaDataBed_Buffer : register(u2);
cbuffer hist_params : register(b0) { float4 histoData; float4 histoData2; };

groupshared uint g_max;
groupshared float g_lum[1024];

uint2 src_px(uint2 id, uint2 sz)
{
    uint w, h;
    scene_Buffer.GetDimensions(w, h);
    return min((uint2)(((float2)id + 0.5) * float2(w, h) / (float2)sz), uint2(w - 1, h - 1));
}

[numthreads(32, 32, 1)]
void main(uint3 gid : SV_GroupID, uint3 tig : SV_GroupThreadID, uint3 id : SV_DispatchThreadID)
{
    uint2 sz = (uint2)histoData.zw;
    bool first = tig.x == 0 && tig.y == 0;
    if (first) {
        g_max = 0;
        for (uint i = 0; i < 1024; i++) g_lum[i] = -1.0;
    }
    GroupMemoryBarrierWithGroupSync();
    if (id.x < sz.x && id.y < sz.y) {
        float3 c = scene_Buffer.Load(int3(src_px(id.xy, sz), 0)).rgb;
        float l = min(dot(c, float3(0.2125, 0.7154, 0.0721)), 10.0);
        g_lum[tig.y * 32 + tig.x] = l;
        uint q = (uint)round(l * 6553.0);
        InterlockedMax(g_max, q);
    }
    GroupMemoryBarrierWithGroupSync();
    if (first) {
        float s = 0;
        uint n = 0;
        for (uint i = 0; i < 1024; i++)
            if (g_lum[i] >= 0) { s += g_lum[i]; n++; }
        float avg = n > 0 ? s / (float)n : s;
        histogramMetaDataBed_Buffer[gid.xy] = (g_max << 16) | (uint)round(avg * 6553.0);
    }
}
