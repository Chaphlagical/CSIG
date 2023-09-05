#include "common.slangh"

#define LOCAL_SIZE 8
#define CUBEMAP_SIZE 1024
#define IRRADIANCE_CUBEMAP_SIZE 128
#define SH_INTERMEDIATE_SIZE (IRRADIANCE_CUBEMAP_SIZE / LOCAL_SIZE)
#define CUBEMAP_MIP_LEVEL 2.0

[[vk::binding(0, 0)]] TextureCube<float4> EnvMap;
[[vk::binding(0, 0)]] SamplerState Sampler;
[[vk::binding(1, 0)]] RWTexture2DArray<float4> SHIntermediate;

groupshared SH9Color sh_coeffs[LOCAL_SIZE][LOCAL_SIZE];
groupshared float sh_weights[LOCAL_SIZE][LOCAL_SIZE];

[shader("compute")]
[numthreads(LOCAL_SIZE, LOCAL_SIZE, 1)]
void main(CSParam param)
{
    for (int i = 0; i < 9; i++)
    {
            sh_coeffs[param.GroupThreadID.x][param.GroupThreadID.y].weights[i] = float3(0.0);
    }

    GroupMemoryBarrierWithGroupSync();

    uint width = uint(CUBEMAP_SIZE * pow(0.5, CUBEMAP_MIP_LEVEL));
    uint height = uint(CUBEMAP_SIZE * pow(0.5, CUBEMAP_MIP_LEVEL));

    // Generate spherical harmonics basis
    float3  dir         = calculate_cubemap_direction(param.DispatchThreadID.z, param.DispatchThreadID.x, param.DispatchThreadID.y, width, height);
    float solid_angle = calculate_solid_angle(param.DispatchThreadID.x, param.DispatchThreadID.y, width, height);
    float3  texel       = EnvMap.SampleLevel(Sampler, dir, CUBEMAP_MIP_LEVEL).rgb;

    SH9 basis = project_sh(dir);

    sh_weights[param.GroupThreadID.x][param.GroupThreadID.y] = solid_angle;

    for (int i = 0; i < 9; i++)
    {
        sh_coeffs[param.GroupThreadID.x][param.GroupThreadID.y].weights[i] += texel * basis.weights[i] * solid_angle;
    }

    GroupMemoryBarrierWithGroupSync();

    if (param.GroupThreadID.x == 0)
    {
        for (int shared_idx = 1; shared_idx < LOCAL_SIZE; shared_idx++)
        {
            sh_weights[0][param.GroupThreadID.y] += sh_weights[shared_idx][param.GroupThreadID.y];

            for (int coef_idx = 0; coef_idx < 9; coef_idx++)
                sh_coeffs[0][param.GroupThreadID.y].weights[coef_idx] += sh_coeffs[shared_idx][param.GroupThreadID.y].weights[coef_idx];
        }
    }

    GroupMemoryBarrierWithGroupSync();

    if (param.GroupThreadID.x == 0 && param.GroupThreadID.y == 0)
    {
        for (int shared_idx = 1; shared_idx < LOCAL_SIZE; shared_idx++)
        {
            sh_weights[0][0] += sh_weights[0][shared_idx];

            for (int coef_idx = 0; coef_idx < 9; coef_idx++)
                sh_coeffs[0][0].weights[coef_idx] += sh_coeffs[0][shared_idx].weights[coef_idx];
        }

        for (int coef_idx = 0; coef_idx < 9; coef_idx++)
        {
            int3 p = int3((SH_INTERMEDIATE_SIZE * coef_idx) + (param.DispatchThreadID.x / LOCAL_SIZE), param.DispatchThreadID.y / LOCAL_SIZE, param.DispatchThreadID.z);
            SHIntermediate[p] = float4(sh_coeffs[0][0].weights[coef_idx], sh_weights[0][0]);
        }
    }
}