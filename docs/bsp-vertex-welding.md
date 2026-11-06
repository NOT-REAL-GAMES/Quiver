# BSP vertex welding

The client renderer welds nearby BSP vertex positions when a brush model is
loaded, before its render geometry and meshlets are built. The original BSP
indices are retained. Vertices are only matched within the same brush model;
vertices shared by different models and endpoints of very short edges are
excluded. Lightmap extents are calculated from the original coordinates and
the collision hulls are unchanged.

`r_bsp_weld_epsilon` is the maximum Euclidean displacement in Quake units.
Its default is `0.001`, its maximum effective value is `0.01`, and `0` disables
the pass. Changes take effect the next time the BSP is loaded. Raising the
value can change the shape of deliberately small features.

This only joins nearby existing vertices. It cannot close a T-junction where a
vertex lies in the middle of another face's edge; that would also require
splitting and consistently triangulating the edge.
