# Headless test bench

Runs the renderer's garment code (skeleton stabilisation, garment mesher, cloth solver, occluders,
fabric textures) without Unreal: `shim/` stands in for the handful of engine types that code uses,
and `harness.cpp` drives it with a synthetic skeleton, prints measurements and renders stills with
a small software rasteriser. Needs only a C++17 compiler; Pillow if you want contact sheets.

```bash
./build.sh && mkdir -p out
./harness jitter              # on-screen garment motion for a user standing still, with tracking noise
./harness settle              # how far the cloth lags a sidestep and how fast it settles
./harness turn out/turn 0.5   # user turns side-on; the tracker reports half the true depth differences
./harness topo                # openings of every rest mesh (expect neck, hem and two cuffs / armholes)
./harness stills out/s 0 0    # every garment: <arms pose 0-6> <yaw degrees>, front and three-quarter views
./harness one out/tee 0 3 0 1 # one garment, four views: <garment> <arms> <yaw> <noise 0/1>
./harness tex out/t           # fabric swatches and the drape map
python3 sheet.py out/sheet.png 5 0.6 out/s_*_front.ppm    # contact sheet: <columns> <scale> files...
```

`STAB=0` in the environment turns the skeleton stabilisation off, for comparison. In the stills
the occluders are coloured: sleeved arm pink, torso cyan, everything else skin. Pink or cyan in
front of a garment means an occluder is showing through it (`poke` in the output counts those
pixels; the torso seen through a neckline counts too).

The tracking noise and the "tracker flattens depth" model are assumptions, not measurements of
BlazePose: use the numbers to compare one build with another, not as absolute figures.
