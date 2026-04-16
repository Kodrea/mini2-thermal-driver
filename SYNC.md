# rs300.c Sync Policy

Each platform carries its own copy of `rs300.c`. This is intentional. Changes require testing on real hardware before merging and not all hardware is always available.

## Reference Platform

**rock5bp** is the reference. It is the most recently verified platform (300-frame stream, kernel 6.1.84-8-rk2410, 2026-04-16). Bug fixes and new features should originate here when possible.

## When to Sync

Any commit that touches `src/rs300.c` in any platform triggers a sync check across all platforms before the PR merges.

## How to Check Divergence

```bash
# Diff a platform against rock5bp (the reference)
diff platforms/rock5bp/src/rs300.c platforms/rpi5/src/rs300.c

# Check all platforms at once
for p in rpi4b rpi5 zero3w zero2w; do
    echo "=== $p ==="
    diff platforms/rock5bp/src/rs300.c platforms/$p/src/rs300.c | head -20
done
```

## Platform-Specific Code

Known intentional divergences:

| Block | Platforms | Reason |
|-|-|-|
| `RKMODULE_GET_MODULE_INFO` ioctl | rock5bp, zero3w | Rockchip BSP requirement |
| `rs300_get_mbus_config` | rock5bp, zero3w | Rockchip BSP callback |
| `rs300_g_frame_interval` | rock5bp, zero3w | Rockchip BSP callback |
| `RKMODULE_*` DT property reads in probe | rock5bp, zero3w | Rockchip BSP requirement |

Rockchip platforms (rock5bp, zero3w) also share the same install path: DKMS + overlay + `initcall_blacklist=rkcif_clr_unready_dev`.

When propagating a fix, skip these blocks on non-Rockchip platforms.

## Propagation Process

1. Make and test the fix on the originating platform
2. Run the diff check above to identify what needs to carry across
3. Apply to each platform, skipping platform-specific blocks as noted above
4. Record which platforms were tested vs assumed-compatible in the PR description
