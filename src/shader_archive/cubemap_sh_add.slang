#include "common.slangh"

#define LOCAL_SIZE 8
#define IRRADIANCE_CUBEMAP_SIZE 128
#define SH_INTERMEDIATE_SIZE IRRADIANCE_CUBEMAP_SIZE / LOCAL_SIZE
#define CUBEMAP_FACE_NUM 6

[[vk::binding(0, 0)]] Texture2DArray<float4> SHIntermediate;
[[vk::binding(1, 0)]] RWTexture2D<float4> IrradianceSH;

groupshared float3 sh_coeffs[SH_INTERMEDIATE_SIZE][CUBEMAP_FACE_NUM];
groupshared float sh_weights[SH_INTERMEDIATE_SIZE][CUBEMAP_FACE_NUM];

[shader("compute")]
[numthreads(1, SH_INTERMEDIATE_SIZE, CUBEMAP_FACE_NUM)]
void main(CSParam param)
{
    sh_coeffs[param.DispatchThreadID.y][param.DispatchThreadID.z] = 0.0;
    sh_weights[param.DispatchThreadID.y][param.DispatchThreadID.z] = 0.0;

    GroupMemoryBarrier();

    for (uint i = 0; i < SH_INTERMEDIATE_SIZE; i++)
    {
        int3 p   = int3(param.DispatchThreadID.x * SH_INTERMEDIATE_SIZE + i, param.DispatchThreadID.y, param.DispatchThreadID.z);
        float4  val = SHIntermediate[p];

        sh_coeffs[param.DispatchThreadID.y][param.DispatchThreadID.z] += val.rgb;
        sh_weights[param.DispatchThreadID.y][param.DispatchThreadID.z] += val.a;
    }

    GroupMemoryBarrier();

    if (param.DispatchThreadID.z == 0)
    {
        for (uint i = 1; i < CUBEMAP_FACE_NUM; i++)
        {
            sh_coeffs[param.DispatchThreadID.y][0] += sh_coeffs[param.DispatchThreadID.y][i];
            sh_weights[param.DispatchThreadID.y][0] += sh_weights[param.DispatchThreadID.y][i];
        }
    }

    GroupMemoryBarrier();

    if (param.DispatchThreadID.y == 0 && param.DispatchThreadID.z == 0)
    {
        for (uint i = 1; i < SH_INTERMEDIATE_SIZE; i++)
        {
            sh_coeffs[0][0] += sh_coeffs[i][0];
            sh_weights[0][0] += sh_weights[i][0];
        }

        float scale = (4.0 * PI) / sh_weights[0][0];

        IrradianceSH[int2(param.DispatchThreadID.x, 0)] = float4(sh_coeffs[0][0] * scale, sh_weights[0][0]);
    }
}