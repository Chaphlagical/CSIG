# 技术报告

本渲染器基于 Vulkan 图形 API 进行光栅化与光线追踪求交。

本渲染器拥有两套管线，**混合管线**和**路径追踪管线**。 下面逐一进行说明。

## 混合管线

混合管线的渲染流程如下：

```mermaid
graph TD
    A[GBuffer] --> di_entry(DI)
    A --> M(AO)
    A --> C(GI)
    A --> D(Reflection)
    di_entry --> E[Composite]
    C --> E
    D --> E
    M --> E
    E --> F[TAA]
    F --> G[AMD FSR]
    G --> H[Tonemapping]
```

### GBuffer

输出为三张 RGBA 贴图
- GBufferA: `base_color.rgb`, `roughness_metallic.g`
- GBufferB: `normal`, `motion_vector`
- GBufferC: `roughness_metallic.r`, `curvature`, `instance_id`, `linear_z`

其中 normal 采用 octahedral 表示.

### DI (Direct Illumination)

```mermaid
graph LR
    subgraph "DI"
        di_temporal_pass[DI Temporal Reuse] --> di_spatial_pass[DI Spatial Reuse]
        di_spatial_pass --> di_composite_pass[DI Composite]
        di_composite_pass --> di_reprojection_pass[DI Reprojection]
        di_reprojection_pass --> di_denoise_pass[DI Denoise]
        di_denoise_pass --> di_upsampling[DI Upsampling]
    end
```



### GI (Global Illumination)

```mermaid
graph LR
    subgraph "GI"
        gi_raytraced[GI Raytrace] --> gi_probe_update[GI Probe Update]
        gi_probe_update --> gi_border_update[GI Border Update]
        gi_border_update --> gi_sample_probe_grid[GI Probe Grid Sample]
    end
```





### Reflection

```mermaid
graph LR
    subgraph "Reflection"
        refl_trace[Reflection Raytrace] --> refl_reproj[Reflection Reprojection]
        refl_reproj --> refl_denoise[Reflection Denoise]
        refl_denoise --> refl_upsampling[Reflection Upsampling]
    end
```




### AO (Ambient Occlusion)

```mermaid
graph LR
    subgraph "AO"
        ao_trace[AO Raytrace] --> ao_temporal[AO Temporal Accumulation]
        ao_temporal --> ao_blur[AO Bilateral Blur]
        ao_blur --> ao_upsampling[AO Upsampling]
    end
```

### Composite



### TAA

### FSR

### Tonemapping

