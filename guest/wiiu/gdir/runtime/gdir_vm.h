// MIT. GDIR VM: runs the compiled GDScript subset (see gdir_ops.h) for the
// scripted nodes of the current pack. Not the Godot VM.

#ifndef BLAZIUM_PS2_GDIR_VM_H
#define BLAZIUM_PS2_GDIR_VM_H

#ifdef __cplusplus
extern "C" {
#endif

// blob points at the "GDIR" chunk inside SCRP. Returns 1 when it parsed.
int gdir_vm_init(const unsigned char *blob, unsigned sz);
// 1 while a GDIR program drives the scene (pad orbit / legacy tape are off).
int gdir_vm_active(void);
// Runs _process for the current pack's instances, then pushes node
// transforms/visibility to gs_draw and the camera. Handles pack changes.
void gdir_vm_process(float delta);
// Last runtime error ("" when none), for logs.
const char *gdir_vm_error(void);

#ifdef __cplusplus
}
#endif

#endif
