# Amphora AHB swapchain (knife13)

Canonical write-up lives in the Amphora app repo:

`amphora` `docs/13-AHB-IMPORT-PRESENT.md` on branch `main` (commit family `344f731`+).

This tree (`proton_11.0`, commit `1c62dd9a8ba`):

- `win32u/vulkan.c` — `amphora_bind_device_wsi`, `amphora_note_acquire_signal` / flush, Present wait+fence
- `wineandroid.drv/vulkan.c` — `amphora_wine_vk*` IPC to `wsi-sc-<pid>.sock`
- `amphora_ahb_sc.inc` / `amphora_wsi_bridge.c` — twins of Amphora native

Gate passed 2026-09-14 on HA262AAH: Present≥50 + guest-readback MAGENTA, no CPU fill.
Next: HWND Surface zero-copy; do not regress AHB import; no GB/VkLayer/table scan.
