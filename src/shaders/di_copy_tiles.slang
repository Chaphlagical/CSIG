#include "common.slangh"

#define NUM_THREADS_X 8
#define NUM_THREADS_Y 8

[[vk::binding(0, 0)]] RWTexture2D<float4> Output_Image;
[[vk::binding(1, 0)]] Texture2D<float4> Input_Image;
[[vk::binding(2, 0)]] StructuredBuffer<int2> CopyTileData;

[shader("compute")]
[numthreads(NUM_THREADS_X, NUM_THREADS_Y, 1)]
void main(CSParam param)
{
    int2 coord = CopyTileData[param.GroupID.x] + int2(param.GroupThreadID.xy);
    Output_Image[coord] = Input_Image.Load(int3(coord, 0));
}