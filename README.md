# CSIG
第四届CSIG图像图形技术挑战赛-AMD赛道：光照渲染及渲染优化

## 编译与运行

项目使用[xmake](https://xmake.io/#/)进行构建，成功安装xmake后编译项目：

```
xmake -y
```

## 项目简介

* 场景支持：gltf、glb文件
* 材质模型：PBR材质
* 光追算法：
  * 路径追踪管线
    * 光源重要性采样和材质重要性采样
    * 多重重要性采样
    * NEE
  * 混合管线
    * GBuffer：由光栅化管线生成，使用Indirect Draw进行优化
    * 直接光照：ReSTIR DI面光源直接光采样
    * 环境光遮蔽：RTAO
    * 全局光照：DDGI
    * 镜面反射：RTR
    * 超采样：FSR1

## 示例：

RTAO：

