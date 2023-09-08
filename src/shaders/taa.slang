#include "common.slangh"
#include "gbuffer.slangh"
#include "random.slangh"
#include "scene.slangh"

#define NUM_THREADS_X 8
#define NUM_THREADS_Y 8

#define NUM_THREADS 32
#define USE_DILATION
#define MINMAX_3X3_ROUNDED
#define USE_CLIPPING
#define UNJITTER_REPROJECTION
#define UNJITTER_COLORSAMPLES
#define UNJITTER_NEIGHBORHOOD
#define HDR_CORRECTION

struct PushConstant
{
    float4 TimeParams;
    float4 TexelSize;
    float FeedbackMin;
    float FeedbackMax;
    int Sharpen;
};

[[vk::binding(0, 2)]] Texture2D<float4> Current_Image;
[[vk::binding(0, 3)]] RWTexture2D<float4> Output_Image;
[[vk::binding(1, 3)]] Texture2D<float4> Prev_Image;
[[vk::push_constant]] ConstantBuffer<PushConstant> push_constant;

// layout(push_constant) uniform PushConstants
// {
//     float4 u_TexelSize;
//     float4 ViewBuffer.jitter;
//     float4 u_TimeParams;
//     float u_FeedbackMin;
//     float u_FeedbackMax;
//     int u_Sharpen;
// };

// ------------------------------------------------------------------
// CONSTANTS  -------------------------------------------------------
// ------------------------------------------------------------------

static const float FLT_EPS = 0.00000001;

float3 RGB_YCoCg(float3 c)
{
    // Y = R/4 + G/2 + B/4
    // Co = R/2 - B/2
    // Cg = -R/4 + G/2 - B/4
    return float3(
        c.x / 4.0 + c.y / 2.0 + c.z / 4.0,
        c.x / 2.0 - c.z / 2.0,
        -c.x / 4.0 + c.y / 2.0 - c.z / 4.0);
}

float3 YCoCg_RGB(float3 c)
{
    // R = Y + Co - Cg
    // G = Y + Cg
    // B = Y - Co - Cg
    return clamp(float3(
                     c.x + c.y - c.z,
                     c.x + c.z,
                     c.x - c.y - c.z),
                 0.0,
                 1.0);
}

float4 sample_color(Texture2D tex, float2 uv)
{
#ifdef USE_YCOCG
    float4 c = tex.SampleLevel(Samplers[int(SamplerType::Linear)], uv, 0.0);
    return float4(RGB_YCoCg(c.rgb), c.a);
#else
    return tex.SampleLevel(Samplers[int(SamplerType::Linear)], uv, 0.0);
#endif
}

float3 resolve_color(float3 c)
{
#ifdef USE_YCOCG
    return YCoCg_RGB(c.rgb).rgb;
#else
    return c;
#endif
}

float4 clip_aabb(float3 aabb_min, float3 aabb_max, float4 p, float4 q)
{
#ifdef USE_OPTIMIZATIONS
    // note: only clips towards aabb center (but fast!)
    float3 p_clip = 0.5 * (aabb_max + aabb_min);
    float3 e_clip = 0.5 * (aabb_max - aabb_min) + FLT_EPS;
    float4 v_clip = q - float4(p_clip, p.w);
    float3 v_unit = v_clip.xyz / e_clip;
    float3 a_unit = abs(v_unit);
    float ma_unit = max(a_unit.x, max(a_unit.y, a_unit.z));
    if (ma_unit > 1.0)
        return float4(p_clip, p.w) + v_clip / ma_unit;
    else
        return q; // point inside aabb
#else
    float4 r = q - p;
    float3 rmax = aabb_max - p.xyz;
    float3 rmin = aabb_min - p.xyz;
    const float eps = FLT_EPS;
    if (r.x > rmax.x + eps)
        r *= (rmax.x / r.x);
    if (r.y > rmax.y + eps)
        r *= (rmax.y / r.y);
    if (r.z > rmax.z + eps)
        r *= (rmax.z / r.z);
    if (r.x < rmin.x - eps)
        r *= (rmin.x / r.x);
    if (r.y < rmin.y - eps)
        r *= (rmin.y / r.y);
    if (r.z < rmin.z - eps)
        r *= (rmin.z / r.z);
    return p + r;
#endif
}

// ------------------------------------------------------------------

float3 find_closest_fragment_3x3(float2 uv)
{
    float2 dd = abs(push_constant.TexelSize.xy);
    float2 du = float2(dd.x, 0.0);
    float2 dv = float2(0.0, dd.y);

    float3 dtl = float3(-1, -1, DepthBuffer.SampleLevel(Samplers[int(SamplerType::Linear)], uv - dv - du, 0).x);
    float3 dtc = float3(0, -1, DepthBuffer.SampleLevel(Samplers[int(SamplerType::Linear)], uv - dv, 0).x);
    float3 dtr = float3(1, -1, DepthBuffer.SampleLevel(Samplers[int(SamplerType::Linear)], uv - dv + du, 0).x);

    float3 dml = float3(-1, -1, DepthBuffer.SampleLevel(Samplers[int(SamplerType::Linear)], uv - du, 0).x);
    float3 dmc = float3(0, -1, DepthBuffer.SampleLevel(Samplers[int(SamplerType::Linear)], uv, 0).x);
    float3 dmr = float3(1, -1, DepthBuffer.SampleLevel(Samplers[int(SamplerType::Linear)], uv + du, 0).x);

    float3 dbl = float3(-1, 1, DepthBuffer.SampleLevel(Samplers[int(SamplerType::Linear)], uv + dv - du, 0).x);
    float3 dbc = float3(0, 1, DepthBuffer.SampleLevel(Samplers[int(SamplerType::Linear)], uv + dv, 0).x);
    float3 dbr = float3(1, 1, DepthBuffer.SampleLevel(Samplers[int(SamplerType::Linear)], uv + dv + du, 0).x);

    float3 dmin = dtl;
    if (dmin.z > dtc.z) dmin = dtc;
    if (dmin.z > dtr.z) dmin = dtr;

    if (dmin.z > dml.z) dmin = dml;
    if (dmin.z > dmc.z) dmin = dmc;
    if (dmin.z > dmr.z) dmin = dmr;

    if (dmin.z > dbl.z) dmin = dbl;
    if (dmin.z > dbc.z) dmin = dbc;
    if (dmin.z > dbr.z) dmin = dbr;

    return float3(uv + dd.xy * dmin.xy, dmin.z);
}

// ------------------------------------------------------------------

float2 sample_velocity_dilated(Texture2D tex, float2 uv, int support)
{
    float2 du = float2(push_constant.TexelSize.x, 0.0);
    float2 dv = float2(0.0, push_constant.TexelSize.y);
    float2 mv = float2(0.0);
    float rmv = 0.0;
    int end = support + 1;

    for (int i = -support; i != end; i++)
    {
        for (int j = -support; j != end; j++)
        {
            float2 v = tex.SampleLevel(Samplers[int(SamplerType::Linear)], uv + i * dv + j * du, 0).xy;
            float rv = dot(v, v);
            if (rv > rmv)
            {
                mv = v;
                rmv = rv;
            }
        }
    }
    return mv;
}

// ------------------------------------------------------------------

float4 sample_color_motion(Texture2D tex, float2 uv, float2 ss_vel)
{
    const float2 v = 0.5 * ss_vel;
    const int taps = 3; // on either side!
    float srand = srand(uv + push_constant.TimeParams.yy);
    float2 vtap = v / taps;
    float2 pos0 = uv + vtap * (0.5 * srand);
    float4 accu = float4(0.0);
    float wsum = 0.0;

    for (int i = -taps; i <= taps; i++)
    {
        float w = 1.0; // box
        // float w = taps - abs(i) + 1;// triangle
        // float w = 1.0 / (1 + abs(i));// pointy triangle
        accu += w * sample_color(tex, pos0 + i * vtap);
        wsum += w;
    }
    return accu / wsum;
}

// ------------------------------------------------------------------

float3 tonemap(float3 x)
{
    return x / (x + float3(1.0f)); // Reinhard tonemap
}

// ------------------------------------------------------------------

float3 inverse_tonemap(float3 x)
{
    return x / max((float3(1.0f) - x), float3(FLT_EPS));
}

// ------------------------------------------------------------------

float3 temporal_reprojection(float2 ss_txc, float2 ss_vel, float vs_dist)
{
    // read texels
#if defined(UNJITTER_COLORSAMPLES)
    float4 texel0 = sample_color(Current_Image, ss_txc + ViewBuffer.jitter.xy);
#else
    float4 texel0 = sample_color(Current_Image, ss_txc);
#endif

    float4 texel1 = sample_color(Prev_Image, ss_txc + ss_vel);

    // calc min-max of current neighbourhood
#if defined(UNJITTER_NEIGHBORHOOD)
    float2 uv = ss_txc + ViewBuffer.jitter.xy;
#else
    float2 uv = ss_txc;
#endif

#if defined(MINMAX_3X3) || defined(MINMAX_3X3_ROUNDED)
    float2 du = float2(push_constant.TexelSize.x, 0.0);
    float2 dv = float2(0.0, push_constant.TexelSize.y);
    float4 ctl = sample_color(Current_Image, uv - dv - du);
    float4 ctc = sample_color(Current_Image, uv - dv);
    float4 ctr = sample_color(Current_Image, uv - dv + du);
    float4 cml = sample_color(Current_Image, uv - du);
    float4 cmc = sample_color(Current_Image, uv);
    float4 cmr = sample_color(Current_Image, uv + du);
    float4 cbl = sample_color(Current_Image, uv + dv - du);
    float4 cbc = sample_color(Current_Image, uv + dv);
    float4 cbr = sample_color(Current_Image, uv + dv + du);
    float4 cmin = min(ctl, min(ctc, min(ctr, min(cml, min(cmc, min(cmr, min(cbl, min(cbc, cbr))))))));
    float4 cmax = max(ctl, max(ctc, max(ctr, max(cml, max(cmc, max(cmr, max(cbl, max(cbc, cbr))))))));

#if defined(MINMAX_3X3_ROUNDED) || defined(USE_YCOCG) || defined(USE_CLIPPING)
    float4 cavg = (ctl + ctc + ctr + cml + cmc + cmr + cbl + cbc + cbr) / 9.0;
#endif

#if defined(MINMAX_3X3_ROUNDED)
    float4 cmin5 = min(ctc, min(cml, min(cmc, min(cmr, cbc))));
    float4 cmax5 = max(ctc, max(cml, max(cmc, max(cmr, cbc))));
    float4 cavg5 = (ctc + cml + cmc + cmr + cbc) / 5.0;
    cmin = 0.5 * (cmin + cmin5);
    cmax = 0.5 * (cmax + cmax5);
    cavg = 0.5 * (cavg + cavg5);
#endif

#elif defined(MINMAX_4TAP_VARYING) // this is the method used in v2 (PDTemporalReprojection2)
    const float _SubpixelThreshold = 0.5;
    const float _GatherBase = 0.5;
    const float _GatherSubpixelMotion = 0.1666;
    float2 texel_vel = ss_vel / u_TexelSize.xy;
    float texel_vel_mag = length(texel_vel) * vs_dist;
    float k_subpixel_motion = clamp(_SubpixelThreshold / (FLT_EPS + texel_vel_mag), 0.0, 1.0);
    float k_min_max_support = _GatherBase + _GatherSubpixelMotion * k_subpixel_motion;
    float2 ss_offset01 = k_min_max_support * float2(-u_TexelSize.x, u_TexelSize.y);
    float2 ss_offset11 = k_min_max_support * float2(u_TexelSize.x, u_TexelSize.y);
    float4 c00 = sample_color(Current_Image, uv - ss_offset11);
    float4 c10 = sample_color(Current_Image, uv - ss_offset01);
    float4 c01 = sample_color(Current_Image, uv + ss_offset01);
    float4 c11 = sample_color(Current_Image, uv + ss_offset11);
    float4 cmin = min(c00, min(c10, min(c01, c11)));
    float4 cmax = max(c00, max(c10, max(c01, c11)));

#if defined(USE_YCOCG) || defined(USE_CLIPPING)
    float4 cavg = (c00 + c10 + c01 + c11) / 4.0;
#endif
#else
#error "missing keyword MINMAX_..."
#endif

    // shrink chroma min-max
#if defined(USE_YCOCG)
    float2 chroma_extent = float2(0.25 * 0.5 * (cmax.r - cmin.r));
    float2 chroma_center = texel0.gb;
    cmin.yz = chroma_center - chroma_extent;
    cmax.yz = chroma_center + chroma_extent;
    cavg.yz = chroma_center;
#endif

    // clamp to neighbourhood of current sample
#if defined(USE_CLIPPING)
    texel1 = clip_aabb(cmin.xyz, cmax.xyz, clamp(cavg, cmin, cmax), texel1);
#else
    texel1 = clamp(texel1, cmin, cmax);
#endif

    // feedback weight from unbiased luminance diff (t.lottes)
#if defined(USE_YCOCG)
    float lum0 = texel0.r;
    float lum1 = texel1.r;
#else
    float lum0 = luminance(texel0.rgb);
    float lum1 = luminance(texel1.rgb);
#endif
    float unbiased_diff = abs(lum0 - lum1) / max(lum0, max(lum1, 0.2));
    float unbiased_weight = 1.0 - unbiased_diff;
    float unbiased_weight_sqr = unbiased_weight * unbiased_weight;
    float k_feedback = lerp(push_constant.FeedbackMin, push_constant.FeedbackMax, unbiased_weight_sqr);

    // sharpen
    if (push_constant.Sharpen == 1)
    {
        float4 sum = float4(0.0);

        sum += -1.0 * cml;
        sum += -1.0 * ctc;
        sum += 5.0 * texel0;
        sum += -1.0 * cbc;
        sum += -1.0 * cmr;

        texel0 = sum;
    }

#if defined(HDR_CORRECTION)
    texel0.rgb = tonemap(texel0.rgb);
    texel1.rgb = tonemap(texel1.rgb);
#endif

    // output
    float3 blended = lerp(texel0.rgb, texel1.rgb, k_feedback);

#if defined(HDR_CORRECTION)
    blended = inverse_tonemap(blended);
#endif

    return blended;
}

// ------------------------------------------------------------------
// MAIN  ------------------------------------------------------------
// ------------------------------------------------------------------

[shader("compute")]
[numthreads(NUM_THREADS_X, NUM_THREADS_Y, 1)]
void main(CSParam param)
{
    float2 tex_coord = (float2(param.DispatchThreadID.xy) + 0.5f) * push_constant.TexelSize.xy;

#if defined(UNJITTER_REPROJECTION)
    float2 uv = tex_coord + ViewBuffer.jitter.xy;
#else
    float2 uv = tex_coord;
#endif

#if defined(USE_DILATION)
        //--- 3x3 norm (sucks)
        // float2 ss_vel = sample_velocity_dilated(s_Velocity, uv, 1);
        // float vs_dist = depth_sample_linear(uv);
        //--- 5 tap nearest (decent)
        // float3 c_frag = find_closest_fragment_5tap(uv);
        // float2 ss_vel = texture(s_Velocity, c_frag.xy).zw;
        // float vs_dist = depth_resolve_linear(c_frag.z);
        //--- 3x3 nearest (good)
    float3 c_frag = find_closest_fragment_3x3(uv);
    float2 ss_vel = GBufferB.SampleLevel(Samplers[int(SamplerType::Linear)], c_frag.xy, 0).zw;
    float vs_dist = c_frag.z;
#else
    float2 ss_vel = GBufferB.SampleLevel(Samplers[int(SamplerType::Linear)], uv, 0).zw;
    float vs_dist = DepthBuffer.SampleLevel(Samplers[int(SamplerType::Linear)], uv, 0).r;
#endif
    // temporal resolve
    float3 color_temporal = temporal_reprojection(tex_coord, ss_vel, vs_dist);
    // prepare outputs
    float3 to_buffer = resolve_color(color_temporal);

    Output_Image[param.DispatchThreadID.xy] = float4(clamp(to_buffer, 0.0, 1.0).xyz, 1.0);
}

// ------------------------------------------------------------------