# UEGaussianSplatting (WIP)

> Still Work In Progress! Not Done Yet!

## Overview
Loading and rendering highly detailed large scale assets in 3D software is always a big challenge.
Unreal Engine has the feature like Nanite for handling high poly/large scale mesh.
However, currently there isn't any solution for handling high density/large scale gaussian splats.
This project primarily aims to solve this issue.


## To Do List
- [x] Import, process PLY file and save as uasset
- [x] Render splats at given postiion
- [x] Add efficient sorting algorith: Radix Sort
- [x] Render color and alpha blending
- [ ] Hook render result into Unreal Engine's render pipeline: before post process
- [ ] It can render gaussian splatting now
- [ ] Dynamic load and unload macro tiles
- [ ] Dynamic switching micro LOD 
